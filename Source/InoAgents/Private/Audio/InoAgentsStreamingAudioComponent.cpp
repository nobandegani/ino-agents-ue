// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoAgentsStreamingAudioComponent.h"

#include "InoAgentsAudioMp3Decoder.h"
#include "InoAgentsLog.h"

#include "Sound/SoundWaveProcedural.h"

UInoAgentsStreamingAudioComponent::UInoAgentsStreamingAudioComponent(
    const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // Tick is enabled only while a stream is active so an idle
    // component is free. Poll-based queue-drained detection is
    // intentional — see the class-header explanation.
    PrimaryComponentTick.bCanEverTick          = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;

    // Streaming audio is almost always UI / character voice, not
    // diegetic ambience, so default to auto-activate off. Users
    // who want it spatialized can still attach it to an actor with
    // a transform and set attenuation; that stays fully supported.
    bAutoActivate = false;

    // Build the procedural wave subobject at CDO-construction time so
    // every instance gets its own. SetSound binds it to this component
    // (inherited UAudioComponent::Sound UPROPERTY). We hide the Sound
    // category in UCLASS metadata so designers don't accidentally
    // swap it in the details panel.
    ProceduralWave = ObjectInitializer.CreateDefaultSubobject<USoundWaveProcedural>(
        this, TEXT("ProceduralWave"));
    if (ProceduralWave != nullptr)
    {
        // UE 5.7 made USoundWave::SampleRate protected — use the public
        // SetSampleRate setter instead of direct assignment. NumChannels,
        // Duration, SoundGroup, and bLooping are still public.
        ProceduralWave->SetSampleRate(static_cast<uint32>(PcmSampleRate));
        ProceduralWave->NumChannels = PcmNumChannels;
        ProceduralWave->Duration    = INDEFINITELY_LOOPING_DURATION;
        ProceduralWave->SoundGroup  = SOUNDGROUP_Default;
        ProceduralWave->bLooping    = false;
        SetSound(ProceduralWave);

        // Mirror the default PCM format into the active-format tracking
        // so the very first FeedAudioBytes for a 44100/mono PCM stream
        // can hit the fast path (Play without rebuild). Mismatches
        // still go through the rebuild branch in EnsureProceduralWave.
        ActiveSampleRate  = PcmSampleRate;
        ActiveNumChannels = PcmNumChannels;
    }
}

void UInoAgentsStreamingAudioComponent::BeginDestroy()
{
    // Drop the decoder state explicitly before the UObject path
    // finalises this component. TPimplPtr would handle it in the
    // dtor too, but calling Reset here means the decoder is gone
    // by the time any late queue callback fires from the audio
    // engine on shutdown.
    Mp3State.Reset();

    Super::BeginDestroy();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void UInoAgentsStreamingAudioComponent::SetPcmFormat(int32 SampleRateHz, int32 NumChannels)
{
    if (bStreamActive)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("UInoAgentsStreamingAudioComponent::SetPcmFormat called mid-stream; "
                    "ignored. Call before the first FeedAudioBytes of a stream."));
        return;
    }

    PcmSampleRate  = FMath::Clamp(SampleRateHz, 8000, 192000);
    PcmNumChannels = FMath::Clamp(NumChannels, 1, 2);
}

void UInoAgentsStreamingAudioComponent::FeedAudioBytes(
    const TArray<uint8>& AudioBytes, EInoAgentsAudioFormat Format)
{
    if (AudioBytes.Num() == 0)
    {
        return;
    }

    // Format mismatch mid-stream -> wrap up the prior one cleanly
    // before switching. ResetInternalState drops the procedural-wave
    // queue so the new stream starts from silence.
    if (bStreamActive && Format != CurrentFormat)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("UInoAgentsStreamingAudioComponent: format change mid-stream "
                    "(%d -> %d); resetting"),
               static_cast<int32>(CurrentFormat), static_cast<int32>(Format));
        ResetInternalState();
    }

    BeginStreamIfNeeded(Format);

    switch (Format)
    {
        case EInoAgentsAudioFormat::PcmInt16:
        {
            // int16 little-endian is USoundWaveProcedural's native
            // input format — bytes queue straight through with no
            // conversion. An odd byte count is truncated to the
            // nearest sample boundary; in practice callers always
            // feed whole samples.
            EnsureProceduralWave(PcmSampleRate, PcmNumChannels);
            if (ProceduralWave != nullptr)
            {
                const int32 ByteCount = AudioBytes.Num() & ~1;  // round down to 2-byte boundary
                if (ByteCount > 0)
                {
                    ProceduralWave->QueueAudio(AudioBytes.GetData(), ByteCount);
                    FireReadyToPlayIfFirstBytes();
                }
            }
            break;
        }

        case EInoAgentsAudioFormat::PcmFloat32:
        {
            // Convert float32 samples (assumed in [-1.0, +1.0]) into
            // int16 before queueing. Out-of-range samples are clamped.
            // An odd byte count is truncated to the nearest 4-byte
            // sample boundary.
            EnsureProceduralWave(PcmSampleRate, PcmNumChannels);
            if (ProceduralWave != nullptr)
            {
                const int32 NumFloats = AudioBytes.Num() / static_cast<int32>(sizeof(float));
                if (NumFloats > 0)
                {
                    const float* Floats = reinterpret_cast<const float*>(AudioBytes.GetData());
                    TArray<int16> Int16Samples;
                    Int16Samples.SetNumUninitialized(NumFloats);
                    for (int32 i = 0; i < NumFloats; ++i)
                    {
                        const float Clamped = FMath::Clamp(Floats[i], -1.0f, 1.0f);
                        Int16Samples[i] = static_cast<int16>(Clamped * 32767.0f);
                    }
                    QueuePcmInt16(Int16Samples.GetData(), NumFloats);
                    FireReadyToPlayIfFirstBytes();
                }
            }
            break;
        }

        case EInoAgentsAudioFormat::Mp3:
        {
            DecodeAndQueueMp3(AudioBytes);
            break;
        }

        default:
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("UInoAgentsStreamingAudioComponent::FeedAudioBytes: "
                        "unknown format %d"),
                   static_cast<int32>(Format));
            OnError.Broadcast(TEXT("Unknown audio format"));
            ResetInternalState();
            return;
        }
    }
}

void UInoAgentsStreamingAudioComponent::FinalizeStream()
{
    if (!bStreamActive || bStreamFinalized)
    {
        return;
    }

    // For MP3 streams, drain the decoder with an empty feed. This
    // flushes any trailing frame that was fully buffered but hadn't
    // been emitted yet (rare — most feed calls emit everything they
    // can — but cheap insurance).
    if (CurrentFormat == EInoAgentsAudioFormat::Mp3 && Mp3State.IsValid())
    {
        FInoAgentsMp3DecodeResult Tail = InoAgentsAudioMp3::Feed(*Mp3State, TArray<uint8>());
        if (Tail.Pcm.Num() > 0)
        {
            QueuePcmInt16(Tail.Pcm.GetData(), Tail.Pcm.Num());
        }
    }

    bStreamFinalized = true;
    UE_LOG(LogInoAgents, Verbose,
           TEXT("UInoAgentsStreamingAudioComponent: stream finalized; waiting for drain"));
}

void UInoAgentsStreamingAudioComponent::PlayAudio(
    const TArray<uint8>& AudioBytes, EInoAgentsAudioFormat Format)
{
    // One-shot convenience — FeedAudioBytes implicitly starts the
    // stream (and calls Play), FinalizeStream flags it for drain.
    FeedAudioBytes(AudioBytes, Format);
    FinalizeStream();
}

void UInoAgentsStreamingAudioComponent::StopAndReset()
{
    Stop();                 // inherited UAudioComponent::Stop
    ResetInternalState();
}

// ---------------------------------------------------------------------------
// UActorComponent overrides
// ---------------------------------------------------------------------------

void UInoAgentsStreamingAudioComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    // Poll for "stream finalized AND queue fully drained". Cheap —
    // one integer load per frame. Once the condition fires we
    // disable our own tick so idle components cost nothing.
    if (bStreamFinalized && ProceduralWave != nullptr)
    {
        const int32 Available = ProceduralWave->GetAvailableAudioByteCount();
        if (Available == 0)
        {
            UE_LOG(LogInoAgents, Verbose,
                   TEXT("UInoAgentsStreamingAudioComponent: queue drained; firing OnFinished"));

            const bool bWasActive = bStreamActive;
            ResetInternalState();

            if (bWasActive)
            {
                OnFinished.Broadcast();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void UInoAgentsStreamingAudioComponent::BeginStreamIfNeeded(EInoAgentsAudioFormat Format)
{
    if (bStreamActive)
    {
        return;
    }

    CurrentFormat       = Format;
    bStreamActive       = true;
    bStreamFinalized    = false;
    bReadyToPlayFired   = false;

    if (Format == EInoAgentsAudioFormat::Mp3 && !Mp3State.IsValid())
    {
        Mp3State = InoAgentsAudioMp3::CreateState();
    }

    // Enable tick so the drain-poll runs every frame while the
    // stream is active.
    SetComponentTickEnabled(true);
}

void UInoAgentsStreamingAudioComponent::EnsureProceduralWave(int32 SampleRate, int32 NumChannels)
{
    // If the bound procedural wave already matches the target
    // (sample rate + channels), we're done — just call Play so the
    // audio engine starts pulling samples as soon as there are any.
    if (ProceduralWave != nullptr &&
        ActiveSampleRate  == SampleRate &&
        ActiveNumChannels == NumChannels)
    {
        if (!IsPlaying())
        {
            Play();
        }
        return;
    }

    // Mismatch (or first configuration) -> allocate a fresh
    // USoundWaveProcedural. USoundWaveProcedural does NOT accept
    // post-init rate changes; rebuilding from scratch is the
    // supported pattern. The old one is dropped and GC will
    // collect it (the component's UPROPERTY is overwritten).
    UE_LOG(LogInoAgents, Verbose,
           TEXT("UInoAgentsStreamingAudioComponent: (re)building procedural wave at "
                "%d Hz / %d ch"),
           SampleRate, NumChannels);

    ProceduralWave = NewObject<USoundWaveProcedural>(this);
    ProceduralWave->SetSampleRate(static_cast<uint32>(SampleRate));
    ProceduralWave->NumChannels = NumChannels;
    ProceduralWave->Duration    = INDEFINITELY_LOOPING_DURATION;
    ProceduralWave->SoundGroup  = SOUNDGROUP_Default;
    ProceduralWave->bLooping    = false;

    ActiveSampleRate  = SampleRate;
    ActiveNumChannels = NumChannels;

    Stop();                    // release any prior wave binding
    SetSound(ProceduralWave);
    Play();
}

void UInoAgentsStreamingAudioComponent::QueuePcmInt16(const int16* Samples, int32 NumSamples)
{
    if (ProceduralWave == nullptr || NumSamples <= 0 || Samples == nullptr)
    {
        return;
    }
    ProceduralWave->QueueAudio(
        reinterpret_cast<const uint8*>(Samples),
        NumSamples * sizeof(int16));
}

void UInoAgentsStreamingAudioComponent::DecodeAndQueueMp3(const TArray<uint8>& Mp3Bytes)
{
    if (!Mp3State.IsValid())
    {
        // Shouldn't happen — BeginStreamIfNeeded allocates the
        // decoder before FeedAudioBytes reaches us. Defensive check.
        Mp3State = InoAgentsAudioMp3::CreateState();
    }

    const FInoAgentsMp3DecodeResult Result =
        InoAgentsAudioMp3::Feed(*Mp3State, Mp3Bytes);

    if (Result.bError)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsStreamingAudioComponent: MP3 decode error: %s"),
               *Result.ErrorMessage);
        OnError.Broadcast(Result.ErrorMessage);
        ResetInternalState();
        return;
    }

    // First frame of a stream carries the format. Build the
    // procedural wave now, before queueing any samples.
    if (Result.SampleRate > 0 && Result.NumChannels > 0)
    {
        EnsureProceduralWave(Result.SampleRate, Result.NumChannels);
    }

    if (Result.Pcm.Num() > 0)
    {
        QueuePcmInt16(Result.Pcm.GetData(), Result.Pcm.Num());
        FireReadyToPlayIfFirstBytes();
    }
}

void UInoAgentsStreamingAudioComponent::FireReadyToPlayIfFirstBytes()
{
    if (bReadyToPlayFired)
    {
        return;
    }
    if (ProceduralWave == nullptr || ProceduralWave->GetAvailableAudioByteCount() == 0)
    {
        return;
    }
    bReadyToPlayFired = true;
    OnReadyToPlay.Broadcast();
}

void UInoAgentsStreamingAudioComponent::ResetInternalState()
{
    bStreamActive       = false;
    bStreamFinalized    = false;
    bReadyToPlayFired   = false;
    CurrentFormat       = EInoAgentsAudioFormat::PcmInt16;

    // Drop the decoder state so the next stream starts fresh. A
    // brand-new mp3dec_t is cheap (just a zero-init of a small
    // struct).
    Mp3State.Reset();

    // Flush any queued bytes from the procedural wave so a
    // subsequent Play on the same wave starts from silence.
    if (ProceduralWave != nullptr)
    {
        ProceduralWave->ResetAudio();
    }

    SetComponentTickEnabled(false);
}
