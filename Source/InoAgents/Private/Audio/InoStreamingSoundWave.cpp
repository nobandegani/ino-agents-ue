// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoStreamingSoundWave.h"

#include "InoMp3Decoder.h"
#include "InoAgentsLog.h"

#include "Async/Async.h"
#include "Tasks/Pipe.h"

// ---------------------------------------------------------------------------
// RAW → float32 transcoders
// ---------------------------------------------------------------------------
//
// Each source format maps to float32 in the canonical [-1, +1] range
// following the conventions used by Unreal's audio stack and the
// upstream LiteRT-LM / ElevenLabs pipelines:
//
//   Int16   sample / 32768.0f     (full-range signed divisor)
//   Int32   sample / 2147483648.0f
//   UInt8   (sample - 128) / 128.0f
//   Float32 direct copy + clamp (caller-provided bytes may exceed
//                                [-1, +1]; clamp to avoid distortion)
//
// The loops are straightforward — the compiler vectorises them and
// the per-element cost is dwarfed by the async dispatch overhead.

namespace
{
    TArray<float> TranscodeInt16(const uint8* Bytes, int32 NumBytes)
    {
        const int32 NumSamples = NumBytes / static_cast<int32>(sizeof(int16));
        TArray<float> Out;
        Out.SetNumUninitialized(NumSamples);
        const int16* Src = reinterpret_cast<const int16*>(Bytes);
        for (int32 i = 0; i < NumSamples; ++i)
        {
            Out[i] = static_cast<float>(Src[i]) / 32768.0f;
        }
        return Out;
    }

    TArray<float> TranscodeInt32(const uint8* Bytes, int32 NumBytes)
    {
        const int32 NumSamples = NumBytes / static_cast<int32>(sizeof(int32));
        TArray<float> Out;
        Out.SetNumUninitialized(NumSamples);
        const int32* Src = reinterpret_cast<const int32*>(Bytes);
        for (int32 i = 0; i < NumSamples; ++i)
        {
            // Normalise against int32's full range. Using a double
            // intermediate keeps the full 32-bit precision before
            // collapsing to float.
            Out[i] = static_cast<float>(
                static_cast<double>(Src[i]) / 2147483648.0);
        }
        return Out;
    }

    TArray<float> TranscodeUInt8(const uint8* Bytes, int32 NumBytes)
    {
        TArray<float> Out;
        Out.SetNumUninitialized(NumBytes);
        for (int32 i = 0; i < NumBytes; ++i)
        {
            // uint8 PCM is centered at 128, not 0 — subtract the bias
            // before normalising so silence → 0.0f.
            Out[i] = (static_cast<float>(Bytes[i]) - 128.0f) / 128.0f;
        }
        return Out;
    }

    TArray<float> TranscodeFloat32(const uint8* Bytes, int32 NumBytes)
    {
        const int32 NumSamples = NumBytes / static_cast<int32>(sizeof(float));
        TArray<float> Out;
        Out.SetNumUninitialized(NumSamples);
        const float* Src = reinterpret_cast<const float*>(Bytes);
        for (int32 i = 0; i < NumSamples; ++i)
        {
            Out[i] = FMath::Clamp(Src[i], -1.0f, 1.0f);
        }
        return Out;
    }

    /** Dispatch on format. Returns an empty array on unknown format. */
    TArray<float> TranscodeRAW(
        const uint8*              Bytes,
        int32                     NumBytes,
        EInoRawAudioFormat  Format)
    {
        if (Bytes == nullptr || NumBytes <= 0)
        {
            return {};
        }
        switch (Format)
        {
            case EInoRawAudioFormat::Int16:   return TranscodeInt16  (Bytes, NumBytes);
            case EInoRawAudioFormat::Int32:   return TranscodeInt32  (Bytes, NumBytes);
            case EInoRawAudioFormat::UInt8:   return TranscodeUInt8  (Bytes, NumBytes);
            case EInoRawAudioFormat::Float32: return TranscodeFloat32(Bytes, NumBytes);
        }
        return {};
    }
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

UInoStreamingSoundWave::UInoStreamingSoundWave(
    const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // The task pipe serialises append + preallocate work so they
    // land on PCMBuffer in call order regardless of which thread the
    // caller used to enqueue them. Pipe-scoped tasks execute one at
    // a time; ordering is strict.
    AudioTaskPipe = MakeUnique<UE::Tasks::FPipe>(TEXT("InoAgentsStreamingAudio"));
}

void UInoStreamingSoundWave::BeginDestroy()
{
    // Close the visualization gate so any in-flight audio-thread
    // broadcasts short-circuit immediately.
    bActive.Store(false);

    // Drop the pipe. Any task queued on it that hasn't started yet
    // will be cancelled; already-running tasks finish before the
    // pipe destructor returns, which means any lambda we've
    // launched is guaranteed done before our members are gone.
    AudioTaskPipe.Reset();

    Mp3State.Reset();

    Super::BeginDestroy();
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

UInoStreamingSoundWave* UInoStreamingSoundWave::CreateStreamingSoundWave()
{
    // Transient — never saved to disk. NewObject is fine for a
    // runtime-only asset; no DefaultSubobject quirks apply here.
    return NewObject<UInoStreamingSoundWave>();
}

// ---------------------------------------------------------------------------
// Format resolution
// ---------------------------------------------------------------------------

bool UInoStreamingSoundWave::ResolveStreamFormat(
    int32 InSampleRate, int32 InNumChannels)
{
    if (InSampleRate <= 0 || InNumChannels <= 0)
    {
        BroadcastAudioError(FString::Printf(
            TEXT("Invalid source format (%d Hz / %d ch)"),
            InSampleRate, InNumChannels));
        return false;
    }

    if (ConfiguredSampleRate == 0 && ConfiguredNumChannels == 0)
    {
        // First append: lock in the format on the wave.
        ConfiguredSampleRate  = InSampleRate;
        ConfiguredNumChannels = InNumChannels;
        SetSampleRate(static_cast<uint32>(InSampleRate));
        NumChannels = InNumChannels;
        return true;
    }

    if (InSampleRate != ConfiguredSampleRate
     || InNumChannels != ConfiguredNumChannels)
    {
        // v1: resampling / remixing is not implemented. Callers
        // must feed a consistent format per wave. Flagging loudly
        // so the mismatch doesn't silently produce pitched audio.
        BroadcastAudioError(FString::Printf(
            TEXT("Stream format mismatch: wave configured %d Hz / %d ch, "
                 "got %d Hz / %d ch. Call ResetStreamingBuffer or create "
                 "a new wave to switch formats."),
            ConfiguredSampleRate, ConfiguredNumChannels,
            InSampleRate,         InNumChannels));
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Append — RAW
// ---------------------------------------------------------------------------

void UInoStreamingSoundWave::AppendAudioDataFromRAW(
    const TArray<uint8>&      RAWData,
    EInoRawAudioFormat  RAWFormat,
    int32                     InSampleRate,
    int32                     InNumChannels)
{
    if (RAWData.Num() == 0)
    {
        return;
    }
    if (!ResolveStreamFormat(InSampleRate, InNumChannels))
    {
        return;
    }
    if (!AudioTaskPipe.IsValid())
    {
        return;
    }

    // Copy the bytes so the task owns the data — callers often
    // recycle their TArray<uint8> buffer after this returns.
    TArray<uint8> OwnedBytes = RAWData;
    const int32 Channels = FMath::Max(InNumChannels, 1);
    TWeakObjectPtr<UInoStreamingSoundWave> WeakThis(this);

    AudioTaskPipe->Launch(TEXT("AppendAudioDataFromRAW"),
        [WeakThis, OwnedBytes = MoveTemp(OwnedBytes), RAWFormat, Channels]() mutable
        {
            UInoStreamingSoundWave* Self = WeakThis.Get();
            if (Self == nullptr)
            {
                return;
            }

            TArray<float> Floats = TranscodeRAW(
                OwnedBytes.GetData(), OwnedBytes.Num(), RAWFormat);
            if (Floats.Num() == 0)
            {
                return;
            }

            // Drop any trailing partial frame (shouldn't happen if
            // callers send aligned buffers, but be defensive — a
            // non-frame-aligned block would smear channel order on
            // subsequent appends).
            const int32 FrameCount = Floats.Num() / Channels;
            const int32 AlignedCount = FrameCount * Channels;
            if (AlignedCount < Floats.Num())
            {
                Floats.SetNum(AlignedCount, EAllowShrinking::No);
            }

            Self->AppendFloat32Frames(MoveTemp(Floats), FrameCount);
        });
}

// ---------------------------------------------------------------------------
// Append — MP3
// ---------------------------------------------------------------------------

void UInoStreamingSoundWave::AppendAudioDataFromMP3(
    const TArray<uint8>& MP3Data)
{
    if (MP3Data.Num() == 0)
    {
        return;
    }
    if (!AudioTaskPipe.IsValid())
    {
        return;
    }

    TArray<uint8> OwnedBytes = MP3Data;
    TWeakObjectPtr<UInoStreamingSoundWave> WeakThis(this);

    AudioTaskPipe->Launch(TEXT("AppendAudioDataFromMP3"),
        [WeakThis, OwnedBytes = MoveTemp(OwnedBytes)]() mutable
        {
            UInoStreamingSoundWave* Self = WeakThis.Get();
            if (Self == nullptr)
            {
                return;
            }

            // Lazily allocate the decoder state on first feed. We're
            // on the pipe thread, so no other task can race us here.
            if (!Self->Mp3State.IsValid())
            {
                Self->Mp3State = InoMp3::CreateState();
            }

            FInoMp3DecodeResult Decoded =
                InoMp3::Feed(*Self->Mp3State, OwnedBytes);

            if (Decoded.bError)
            {
                Self->BroadcastAudioError(FString::Printf(
                    TEXT("MP3 decode: %s"), *Decoded.ErrorMessage));
                return;
            }

            if (Decoded.SampleRate > 0 && Decoded.NumChannels > 0)
            {
                // First frame of the stream — freeze format on the wave.
                if (!Self->ResolveStreamFormat(Decoded.SampleRate, Decoded.NumChannels))
                {
                    return;
                }
            }

            if (Decoded.Pcm.Num() == 0)
            {
                return;
            }

            // minimp3 returns int16 interleaved. Convert to float32.
            TArray<float> Floats;
            Floats.SetNumUninitialized(Decoded.Pcm.Num());
            for (int32 i = 0; i < Decoded.Pcm.Num(); ++i)
            {
                Floats[i] = static_cast<float>(Decoded.Pcm[i]) / 32768.0f;
            }

            const int32 Channels = FMath::Max(Self->ConfiguredNumChannels, 1);
            const int64 FrameCount = static_cast<int64>(Floats.Num()) / Channels;
            Self->AppendFloat32Frames(MoveTemp(Floats), FrameCount);
        });
}

// ---------------------------------------------------------------------------
// Pre-allocate
// ---------------------------------------------------------------------------

void UInoStreamingSoundWave::PreAllocateAudioData(
    int64 NumOfBytesToPreAllocate,
    const FOnInoPreAllocateAudioDataResult& Result)
{
    FOnInoPreAllocateAudioDataResultNative Native;
    Native.BindLambda(
        [Result](bool bSucceeded) { Result.ExecuteIfBound(bSucceeded); });
    PreAllocateAudioData(NumOfBytesToPreAllocate, Native);
}

void UInoStreamingSoundWave::PreAllocateAudioData(
    int64 NumOfBytesToPreAllocate,
    const FOnInoPreAllocateAudioDataResultNative& Result)
{
    if (!AudioTaskPipe.IsValid())
    {
        Result.ExecuteIfBound(false);
        return;
    }

    const int64 NumFloats = NumOfBytesToPreAllocate / static_cast<int64>(sizeof(float));
    TWeakObjectPtr<UInoStreamingSoundWave> WeakThis(this);

    AudioTaskPipe->Launch(TEXT("PreAllocateAudioData"),
        [WeakThis, NumFloats, Result]()
        {
            UInoStreamingSoundWave* Self = WeakThis.Get();
            bool bOk = false;
            if (Self != nullptr && NumFloats > 0)
            {
                FScopeLock Lock(&Self->SharedPCM->Guard);
                Self->SharedPCM->Data.Reserve(Self->SharedPCM->Data.Num() + NumFloats);
                bOk = true;
            }

            // Result delegate fires on the game thread so BP handlers
            // can touch UObjects safely.
            AsyncTask(ENamedThreads::GameThread,
                [Result, bOk]() { Result.ExecuteIfBound(bOk); });
        });
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void UInoStreamingSoundWave::SetStopSoundOnPlaybackFinish(bool bStop)
{
    FScopeLock Lock(&SharedPCM->Guard);
    bStopSoundOnPlaybackFinish = bStop;
    if (!bStop)
    {
        // Toggling back to "keep playing silence" unlatches any
        // prior broadcast so drain → refill → drain still fires
        // correctly on the next exhaustion.
        bPlaybackFinishedBroadcasted = false;
    }
}

void UInoStreamingSoundWave::ResetStreamingBuffer()
{
    if (!AudioTaskPipe.IsValid())
    {
        return;
    }

    TWeakObjectPtr<UInoStreamingSoundWave> WeakThis(this);
    AudioTaskPipe->Launch(TEXT("ResetStreamingBuffer"),
        [WeakThis]()
        {
            UInoStreamingSoundWave* Self = WeakThis.Get();
            if (Self == nullptr)
            {
                return;
            }

            // Reset the decoder so the next MP3 append starts from a
            // clean rolling buffer rather than mid-frame garbage.
            Self->Mp3State.Reset();

            {
                FScopeLock Lock(&Self->SharedPCM->Guard);
                Self->SharedPCM->Data.Reset();
                Self->SharedPCM->TotalFrames = 0;
                Self->PlayedFrames = 0;
                Self->bPlaybackFinishedBroadcasted = false;
            }
            Self->ResetVisualizationCarry();
        });
}

// ---------------------------------------------------------------------------
// Error plumbing
// ---------------------------------------------------------------------------

void UInoStreamingSoundWave::BroadcastAudioError(const FString& Message)
{
    UE_LOG(LogInoAgents, Error, TEXT("UInoStreamingSoundWave: %s"), *Message);

    TWeakObjectPtr<UInoStreamingSoundWave> WeakThis(this);
    FString Copy = Message;
    AsyncTask(ENamedThreads::GameThread,
        [WeakThis, Copy = MoveTemp(Copy)]()
        {
            UInoStreamingSoundWave* Self = WeakThis.Get();
            if (Self == nullptr)
            {
                return;
            }
            Self->OnAudioErrorNative.Broadcast(Copy);
            Self->OnAudioError.Broadcast(Copy);
        });
}
