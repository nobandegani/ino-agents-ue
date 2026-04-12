// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoAgentsStreamingAudioComponent.h"

#include "InoAgentsAudioMp3Decoder.h"
#include "InoAgentsLog.h"

#include "Sound/SoundWaveProcedural.h"

// Pre-buffer duration is now a UPROPERTY (PreBufferMs) on the
// component, editable in the details panel and from Blueprint.
// Integer milliseconds (e.g. 250) are friendlier than fractional
// seconds (0.25) for designers and Blueprint users.

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

    // IMPORTANT: we do NOT use CreateDefaultSubobject<USoundWaveProcedural>
    // here, even though that would be the idiomatic UE pattern for a
    // CDO-owned child UObject.
    //
    // Reason: USoundWave (the parent of USoundWaveProcedural) owns an
    // AssetImportData UPROPERTY that is editor-only and marked private.
    // When a user wraps this component inside a Blueprint, the Blueprint
    // compiler walks the CDO's subobject tree to build a GEN_VARIABLE
    // template, reaches the ProceduralWave default subobject, and tries
    // to serialise a reference to its AssetImportData. The save fails
    // with:
    //
    //     Illegal reference to private object:
    //     AssetImportData /Script/InoAgents.Default__InoAgentsStreamingAudioComponent
    //         :ProceduralWave.AssetImportData
    //
    // The fix is to keep ProceduralWave null on the CDO and allocate it
    // lazily via NewObject the first time EnsureProceduralWave runs (at
    // the first FeedAudioBytes call). With no default subobject, there
    // is no subobject tree for the Blueprint compiler to traverse, and
    // no chance for the private AssetImportData to leak into a Blueprint
    // asset's serialised state.
    //
    // ProceduralWave remains UPROPERTY(Transient) so it is never saved,
    // and every instance gets a fresh one on first use.
    ProceduralWave    = nullptr;
    ActiveSampleRate  = 0;
    ActiveNumChannels = 0;
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
            // conversion. HTTP chunks can end on any byte so we
            // accumulate the tail in PcmPendingBytes and only emit
            // whole int16 samples (2-byte aligned). The leftover
            // byte (if any) waits for the next feed so sample parity
            // stays correct across chunk boundaries.
            EnsureProceduralWave(PcmSampleRate, PcmNumChannels);
            if (ProceduralWave != nullptr)
            {
                PcmPendingBytes.Append(AudioBytes);
                const int32 WholeSampleBytes = PcmPendingBytes.Num() & ~1;
                if (WholeSampleBytes > 0)
                {
                    ProceduralWave->QueueAudio(PcmPendingBytes.GetData(), WholeSampleBytes);
                    PcmPendingBytes.RemoveAt(0, WholeSampleBytes);
                    TryStartPlayback(/*bForce=*/false);
                }
            }
            break;
        }

        case EInoAgentsAudioFormat::PcmFloat32:
        {
            // Float32 samples are 4 bytes each — same alignment
            // strategy, different boundary. Convert whole floats
            // to int16 with clamping before queueing.
            EnsureProceduralWave(PcmSampleRate, PcmNumChannels);
            if (ProceduralWave != nullptr)
            {
                PcmPendingBytes.Append(AudioBytes);
                const int32 WholeFloatBytes =
                    (PcmPendingBytes.Num() / static_cast<int32>(sizeof(float)))
                    * static_cast<int32>(sizeof(float));
                if (WholeFloatBytes > 0)
                {
                    const int32 NumFloats =
                        WholeFloatBytes / static_cast<int32>(sizeof(float));
                    const float* Floats =
                        reinterpret_cast<const float*>(PcmPendingBytes.GetData());
                    TArray<int16> Int16Samples;
                    Int16Samples.SetNumUninitialized(NumFloats);
                    for (int32 i = 0; i < NumFloats; ++i)
                    {
                        const float Clamped = FMath::Clamp(Floats[i], -1.0f, 1.0f);
                        Int16Samples[i] = static_cast<int16>(Clamped * 32767.0f);
                    }
                    QueuePcmInt16(Int16Samples.GetData(), NumFloats);
                    PcmPendingBytes.RemoveAt(0, WholeFloatBytes);
                    TryStartPlayback(/*bForce=*/false);
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

    // Short-stream case: the pre-buffer threshold was never crossed,
    // so Play() was never called, so the audio is just sitting in the
    // queue. Force-start now so the user hears what we have. This is
    // why PlayAudio(small_buffer) also works — one-shot playback goes
    // through Feed + Finalize, and Finalize unblocks the playback if
    // FeedAudioBytes couldn't.
    TryStartPlayback(/*bForce=*/true);

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

    CurrentFormat     = Format;
    bStreamActive     = true;
    bStreamFinalized  = false;
    bPlaybackStarted  = false;
    PcmPendingBytes.Reset();

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
    // Match: nothing to do. Playback is gated by TryStartPlayback,
    // not by EnsureProceduralWave — we do NOT call Play() here.
    if (ProceduralWave != nullptr &&
        ActiveSampleRate  == SampleRate &&
        ActiveNumChannels == NumChannels)
    {
        return;
    }

    // Mismatch (or first configuration): allocate a fresh
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

    // Pre-buffer target: PreBufferMs worth of int16 samples at the
    // stream's rate and channel count. int16 = 2 bytes per sample
    // per channel. PreBufferMs is a UPROPERTY on the component,
    // editable per-instance in the details panel (integer ms).
    const float Seconds = static_cast<float>(FMath::Max(PreBufferMs, 0)) / 1000.0f;
    PreBufferTargetBytes = static_cast<int32>(
        static_cast<float>(SampleRate * NumChannels * 2) * Seconds);

    // Drop any prior binding and bind the new wave. Play() is NOT
    // called here — TryStartPlayback handles it after enough audio
    // is buffered.
    Stop();
    SetSound(ProceduralWave);
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
        TryStartPlayback(/*bForce=*/false);
    }
}

void UInoAgentsStreamingAudioComponent::TryStartPlayback(bool bForce)
{
    if (bPlaybackStarted || ProceduralWave == nullptr)
    {
        return;
    }

    const int32 Available = ProceduralWave->GetAvailableAudioByteCount();
    if (!bForce && Available < PreBufferTargetBytes)
    {
        // Not enough pre-buffer yet — keep accumulating. The next
        // FeedAudioBytes (or FinalizeStream) will try again.
        return;
    }

    UE_LOG(LogInoAgents, Verbose,
           TEXT("UInoAgentsStreamingAudioComponent: starting playback "
                "(%d bytes buffered, %d-byte target, forced=%d)"),
           Available, PreBufferTargetBytes, bForce ? 1 : 0);

    bPlaybackStarted = true;
    Play();
    OnReadyToPlay.Broadcast();
}

void UInoAgentsStreamingAudioComponent::ResetInternalState()
{
    bStreamActive        = false;
    bStreamFinalized     = false;
    bPlaybackStarted     = false;
    PreBufferTargetBytes = 0;
    PcmPendingBytes.Reset();
    CurrentFormat        = EInoAgentsAudioFormat::PcmInt16;

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
