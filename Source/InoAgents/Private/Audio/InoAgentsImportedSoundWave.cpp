// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoAgentsImportedSoundWave.h"

#include "InoAgentsLog.h"

#include "Async/Async.h"

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

UInoAgentsImportedSoundWave::UInoAgentsImportedSoundWave(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // Streaming waves are indefinitely long from the engine's
    // perspective — the buffer grows / shrinks as data arrives
    // and drains. INDEFINITELY_LOOPING_DURATION tells the audio
    // engine "this never finishes on its own".
    Duration    = INDEFINITELY_LOOPING_DURATION;
    bLooping    = false;
    SoundGroup  = SOUNDGROUP_Default;

    // Sane defaults — mono 44.1 kHz. Subclasses / callers typically
    // override these before the first append via the Initial* setters.
    NumChannels = 1;
    SetSampleRate(44100);
}

void UInoAgentsImportedSoundWave::BeginDestroy()
{
    // Close the visualization gate so any in-flight audio-thread
    // GeneratePCMData short-circuits before dereferencing delegates.
    bActive.Store(false);

    // Drop any partial visualization batch so the next stream (if
    // this wave is reused) doesn't start with stale audio in the
    // carry.
    ResetVisualizationCarry();

    Super::BeginDestroy();
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

UInoAgentsImportedSoundWave* UInoAgentsImportedSoundWave::CreateImportedSoundWave()
{
    return NewObject<UInoAgentsImportedSoundWave>();
}

// ---------------------------------------------------------------------------
// Format
// ---------------------------------------------------------------------------

void UInoAgentsImportedSoundWave::SetInitialDesiredSampleRate(int32 InSampleRate)
{
    const uint32 Clamped = static_cast<uint32>(FMath::Clamp(InSampleRate, 8000, 192000));
    SetSampleRate(Clamped);
}

void UInoAgentsImportedSoundWave::SetInitialDesiredNumChannels(int32 InNumChannels)
{
    NumChannels = FMath::Clamp(InNumChannels, 1, 2);
}

int32 UInoAgentsImportedSoundWave::GetSampleRate() const
{
    return static_cast<int32>(GetSampleRateForCurrentPlatform());
}

int32 UInoAgentsImportedSoundWave::GetNumOfChannels() const
{
    return NumChannels;
}

// ---------------------------------------------------------------------------
// Playback state queries
// ---------------------------------------------------------------------------

float UInoAgentsImportedSoundWave::GetPlaybackTime() const
{
    const int32 Rate = FMath::Max<int32>(GetSampleRate(), 1);
    FScopeLock Lock(&SharedPCM->Guard);
    return static_cast<float>(PlayedFrames) / static_cast<float>(Rate);
}

float UInoAgentsImportedSoundWave::GetDurationSeconds() const
{
    const int32 Rate = FMath::Max<int32>(GetSampleRate(), 1);
    FScopeLock Lock(&SharedPCM->Guard);
    return static_cast<float>(SharedPCM->TotalFrames) / static_cast<float>(Rate);
}

int64 UInoAgentsImportedSoundWave::GetPlayedFrames() const
{
    FScopeLock Lock(&SharedPCM->Guard);
    return PlayedFrames;
}

int64 UInoAgentsImportedSoundWave::GetTotalFrames() const
{
    FScopeLock Lock(&SharedPCM->Guard);
    return SharedPCM->TotalFrames;
}

float UInoAgentsImportedSoundWave::GetPlaybackPercentage() const
{
    FScopeLock Lock(&SharedPCM->Guard);
    if (SharedPCM->TotalFrames <= 0)
    {
        return 0.0f;
    }
    const double Fraction =
        static_cast<double>(PlayedFrames) / static_cast<double>(SharedPCM->TotalFrames);
    return static_cast<float>(FMath::Clamp(Fraction, 0.0, 1.0) * 100.0);
}

bool UInoAgentsImportedSoundWave::IsPlaybackFinished() const
{
    FScopeLock Lock(&SharedPCM->Guard);
    return SharedPCM->TotalFrames > 0 && PlayedFrames >= SharedPCM->TotalFrames;
}

FInoAgentsAudioHeaderInfo UInoAgentsImportedSoundWave::GetAudioHeaderInfo() const
{
    FInoAgentsAudioHeaderInfo Info;
    Info.SampleRate  = GetSampleRate();
    Info.NumChannels = NumChannels;

    FScopeLock Lock(&SharedPCM->Guard);
    Info.TotalFrames = SharedPCM->TotalFrames;

    const int32 Rate = FMath::Max<int32>(Info.SampleRate, 1);
    Info.DurationSeconds =
        static_cast<float>(SharedPCM->TotalFrames) / static_cast<float>(Rate);
    Info.PCMDataSizeBytes =
        static_cast<int64>(SharedPCM->Data.Num()) * static_cast<int64>(sizeof(float));
    return Info;
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

void UInoAgentsImportedSoundWave::RewindPlaybackTime(float StartTime)
{
    const int32 Rate = FMath::Max<int32>(GetSampleRate(), 1);
    const int64 TargetFrame = FMath::Max<int64>(0, static_cast<int64>(StartTime * Rate));

    FScopeLock Lock(&SharedPCM->Guard);
    PlayedFrames = FMath::Min<int64>(TargetFrame, SharedPCM->TotalFrames);
    bPlaybackFinishedBroadcasted = false;
}

void UInoAgentsImportedSoundWave::ReleasePlayedAudioData()
{
    FScopeLock Lock(&SharedPCM->Guard);

    if (PlayedFrames <= 0 || SharedPCM->TotalFrames <= 0)
    {
        return;
    }

    const int32 Channels = FMath::Max(NumChannels, 1);
    const int64 SamplesToDrop = PlayedFrames * Channels;

    // TArray64::RemoveAt shifts trailing elements forward — O(N) but
    // N here is only the unplayed tail, usually small for live
    // capture trim scenarios.
    if (SamplesToDrop > 0 && SamplesToDrop <= SharedPCM->Data.Num())
    {
        SharedPCM->Data.RemoveAt(0, SamplesToDrop, EAllowShrinking::No);
    }

    SharedPCM->TotalFrames -= PlayedFrames;
    PlayedFrames = 0;
    bPlaybackFinishedBroadcasted = false;
}

void UInoAgentsImportedSoundWave::ReleaseMemory()
{
    {
        FScopeLock Lock(&SharedPCM->Guard);
        SharedPCM->Data.Empty();
        SharedPCM->TotalFrames = 0;
        PlayedFrames = 0;
        bPlaybackFinishedBroadcasted = false;
    }
    ResetVisualizationCarry();
}

void UInoAgentsImportedSoundWave::SetSoundLooping(bool bLoop)
{
    FScopeLock Lock(&SharedPCM->Guard);
    bLooping = bLoop;
    if (bLoop)
    {
        // Re-enable the finished broadcast latch so a future
        // non-looping play through can fire it.
        bPlaybackFinishedBroadcasted = false;
    }
}

void UInoAgentsImportedSoundWave::ForceStopPlayback()
{
    // Gate the audio-thread path FIRST so any in-flight
    // GeneratePCMData that re-enters during this call short-circuits
    // cleanly.
    bActive.Store(false);

    FScopeLock Lock(&SharedPCM->Guard);
    SharedPCM->Data.Empty();
    SharedPCM->TotalFrames       = 0;
    PlayedFrames                 = 0;
    bPlaybackFinishedBroadcasted = false;
    VisualizationCarry.Reset();
}

bool UInoAgentsImportedSoundWave::IsSoundLooping() const
{
    FScopeLock Lock(&SharedPCM->Guard);
    return bLooping;
}

// ---------------------------------------------------------------------------
// Post-population transforms
// ---------------------------------------------------------------------------

bool UInoAgentsImportedSoundWave::ResampleSoundWave(int32 NewSampleRate)
{
    const int32 OldRate = GetSampleRate();
    if (NewSampleRate <= 0 || NewSampleRate == OldRate)
    {
        return false;
    }

    FScopeLock Lock(&SharedPCM->Guard);

    if (SharedPCM->TotalFrames <= 0)
    {
        // Nothing to resample yet — just set the rate and return.
        SetSampleRate(static_cast<uint32>(NewSampleRate));
        return true;
    }

    const int32 Channels = FMath::Max(NumChannels, 1);
    const int64 OldFrames = SharedPCM->TotalFrames;
    const int64 NewFrames = FMath::Max<int64>(
        1, (OldFrames * NewSampleRate) / OldRate);

    TArray64<float> Resampled;
    Resampled.SetNumUninitialized(NewFrames * Channels);

    // Linear interpolation. Fine for voice; audible stepping on
    // musical content — callers who need pitch-perfect should
    // resample externally via Audio::FResampler and hand the result
    // in via AppendAudioDataFromRAW.
    const double RateRatio = static_cast<double>(OldFrames - 1)
                           / static_cast<double>(FMath::Max<int64>(NewFrames - 1, 1));

    for (int64 NewFrame = 0; NewFrame < NewFrames; ++NewFrame)
    {
        const double SrcPos    = static_cast<double>(NewFrame) * RateRatio;
        const int64  SrcFloor  = FMath::Min<int64>(static_cast<int64>(SrcPos), OldFrames - 1);
        const int64  SrcCeil   = FMath::Min<int64>(SrcFloor + 1, OldFrames - 1);
        const float  Alpha     = static_cast<float>(SrcPos - static_cast<double>(SrcFloor));

        for (int32 Ch = 0; Ch < Channels; ++Ch)
        {
            const float A = SharedPCM->Data[SrcFloor * Channels + Ch];
            const float B = SharedPCM->Data[SrcCeil  * Channels + Ch];
            Resampled[NewFrame * Channels + Ch] = FMath::Lerp(A, B, Alpha);
        }
    }

    // Scale PlayedFrames proportionally so playback resumes from the
    // same wall-clock position in the audio.
    const int64 RescaledPlayed =
        FMath::Min<int64>((PlayedFrames * NewSampleRate) / OldRate, NewFrames);

    SharedPCM->Data = MoveTemp(Resampled);
    SharedPCM->TotalFrames = NewFrames;
    PlayedFrames = RescaledPlayed;
    bPlaybackFinishedBroadcasted = false;

    SetSampleRate(static_cast<uint32>(NewSampleRate));
    return true;
}

bool UInoAgentsImportedSoundWave::MixSoundWaveChannels(int32 NewNumChannels)
{
    if (NewNumChannels < 1 || NewNumChannels > 2)
    {
        return false;
    }

    FScopeLock Lock(&SharedPCM->Guard);

    const int32 OldChannels = FMath::Max(NumChannels, 1);
    if (OldChannels == NewNumChannels)
    {
        return true;  // no-op
    }
    if (SharedPCM->TotalFrames <= 0)
    {
        NumChannels = NewNumChannels;
        return true;
    }

    const int64 Frames = SharedPCM->TotalFrames;
    TArray64<float> Remixed;
    Remixed.SetNumUninitialized(Frames * NewNumChannels);

    if (OldChannels == 1 && NewNumChannels == 2)
    {
        // Mono → stereo: duplicate each sample into both channels.
        for (int64 F = 0; F < Frames; ++F)
        {
            const float Sample = SharedPCM->Data[F];
            Remixed[F * 2 + 0] = Sample;
            Remixed[F * 2 + 1] = Sample;
        }
    }
    else if (OldChannels == 2 && NewNumChannels == 1)
    {
        // Stereo → mono: average the channels.
        for (int64 F = 0; F < Frames; ++F)
        {
            Remixed[F] =
                (SharedPCM->Data[F * 2 + 0] + SharedPCM->Data[F * 2 + 1]) * 0.5f;
        }
    }
    else
    {
        // Shouldn't reach here given the clamp above.
        return false;
    }

    SharedPCM->Data = MoveTemp(Remixed);
    NumChannels = NewNumChannels;
    return true;
}

void UInoAgentsImportedSoundWave::ReverseAudioBuffer()
{
    FScopeLock Lock(&SharedPCM->Guard);

    const int64 Frames = SharedPCM->TotalFrames;
    if (Frames <= 1)
    {
        return;
    }

    const int32 Channels = FMath::Max(NumChannels, 1);
    // Swap pairs of frames (whole channel blocks at a time, so
    // interleaved stereo stays correctly paired).
    for (int64 F = 0; F < Frames / 2; ++F)
    {
        const int64 Mirror = Frames - 1 - F;
        for (int32 Ch = 0; Ch < Channels; ++Ch)
        {
            Swap(SharedPCM->Data[F * Channels + Ch],
                 SharedPCM->Data[Mirror * Channels + Ch]);
        }
    }

    // Mirror the playback cursor so you stay at the same wall-clock
    // point in the audio, just moving toward the other end.
    PlayedFrames = Frames - PlayedFrames;
    bPlaybackFinishedBroadcasted = false;
}

// ---------------------------------------------------------------------------
// Duplication
// ---------------------------------------------------------------------------

UInoAgentsImportedSoundWave* UInoAgentsImportedSoundWave::DuplicateSoundWave(bool bShareBuffer)
{
    UInoAgentsImportedSoundWave* Dup =
        NewObject<UInoAgentsImportedSoundWave>(GetTransientPackage(), GetClass());
    if (Dup == nullptr)
    {
        return nullptr;
    }

    // Copy format up front — cheap scalars, no lock needed.
    Dup->SetSampleRate(GetSampleRateForCurrentPlatform());
    Dup->NumChannels = NumChannels;
    Dup->Duration    = Duration;
    Dup->SoundGroup  = SoundGroup;
    Dup->bStopSoundOnPlaybackFinish = bStopSoundOnPlaybackFinish;
    Dup->bLooping    = bLooping;

    if (bShareBuffer)
    {
        // Point the new wave at the SAME shared struct. Both waves
        // now see identical data, synchronised via the same Guard.
        Dup->SharedPCM = SharedPCM;
    }
    else
    {
        // Allocate fresh storage and deep-copy the current contents.
        // Hold the source's lock across the copy so no writer can
        // append mid-copy and corrupt the snapshot.
        FScopeLock Lock(&SharedPCM->Guard);
        Dup->SharedPCM = MakeShared<FInoAgentsSharedPCMBuffer>();
        Dup->SharedPCM->Data        = SharedPCM->Data;
        Dup->SharedPCM->TotalFrames = SharedPCM->TotalFrames;
    }

    // Duplicates start at frame 0 regardless of the source's cursor —
    // play the whole thing from the start in the new context.
    Dup->PlayedFrames = 0;
    Dup->bPlaybackFinishedBroadcasted = false;

    return Dup;
}

// ---------------------------------------------------------------------------
// Buffer access
// ---------------------------------------------------------------------------

TArray<float> UInoAgentsImportedSoundWave::GetPCMBuffer() const
{
    FScopeLock Lock(&SharedPCM->Guard);

    const int64 Count = FMath::Min<int64>(SharedPCM->Data.Num(), MAX_int32);
    TArray<float> Out;
    Out.SetNumUninitialized(static_cast<int32>(Count));
    if (Count > 0)
    {
        FMemory::Memcpy(Out.GetData(), SharedPCM->Data.GetData(), Count * sizeof(float));
    }
    return Out;
}

// ---------------------------------------------------------------------------
// Visualization batching
// ---------------------------------------------------------------------------

void UInoAgentsImportedSoundWave::SetNumSamplesPerChunk(int32 NumSamples)
{
    // Accepts INDEX_NONE / 0 as "disable batching" (broadcast whatever
    // the engine polled). Any positive value is accepted; callers who
    // pass silly-small values (e.g. 1) just get a broadcast storm —
    // not incorrect, just wasteful.
    NumSamplesPerChunk = FMath::Max(NumSamples, 0);
}

void UInoAgentsImportedSoundWave::ResetVisualizationCarry()
{
    VisualizationCarry.Reset();
}

// ---------------------------------------------------------------------------
// Append path (called by subclasses)
// ---------------------------------------------------------------------------

void UInoAgentsImportedSoundWave::AppendFloat32Frames(
    TArray<float>&& Interleaved, int64 NumFramesInBlock)
{
    if (NumFramesInBlock <= 0 || Interleaved.Num() == 0)
    {
        return;
    }

    TArray<float> BroadcastCopy;
    const bool bWantPopulateData =
        OnPopulateAudioData.IsBound() || OnPopulateAudioDataNative.IsBound();
    if (bWantPopulateData)
    {
        BroadcastCopy = Interleaved;
    }

    {
        FScopeLock Lock(&SharedPCM->Guard);
        SharedPCM->Data.Append(MoveTemp(Interleaved));
        SharedPCM->TotalFrames += NumFramesInBlock;
        // New data means the wave is no longer exhausted — future
        // playback can fire the finished broadcast again.
        bPlaybackFinishedBroadcasted = false;
    }

    // Re-open the audio-thread gate in case ForceStopPlayback closed
    // it earlier. Without this, a wave that was force-stopped and
    // then re-appended to would still return 0 from GeneratePCMData.
    // Skip the re-open if BeginDestroy has already started — at that
    // point the wave is on its way out and the audio thread shouldn't
    // see us as alive again.
    if (!HasAnyFlags(RF_BeginDestroyed))
    {
        bActive.Store(true);
    }

    TWeakObjectPtr<UInoAgentsImportedSoundWave> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread,
        [WeakThis, Data = MoveTemp(BroadcastCopy), bWantPopulateData]() mutable
        {
            UInoAgentsImportedSoundWave* Self = WeakThis.Get();
            if (Self == nullptr || !Self->bActive.Load())
            {
                return;
            }
            if (bWantPopulateData)
            {
                Self->OnPopulateAudioDataNative.Broadcast(Data);
                Self->OnPopulateAudioData.Broadcast(Data);
            }
            Self->OnPopulateAudioStateNative.Broadcast();
            Self->OnPopulateAudioState.Broadcast();
        });
}

// ---------------------------------------------------------------------------
// Audio render thread
// ---------------------------------------------------------------------------

int32 UInoAgentsImportedSoundWave::GeneratePCMData(
    uint8* OutPCMData, const int32 SamplesNeeded)
{
    // Return-value contract (from USoundWaveProcedural):
    //   > 0  — bytes filled. Source keeps polling for more.
    //   = 0  — end of stream. Source stops.
    //
    // For streaming we want the source to stay alive through buffer
    // underruns, so we pad with silence and return a non-zero byte
    // count. Only when bStopSoundOnPlaybackFinish is true AND the
    // buffer is fully played do we return 0.

    if (OutPCMData == nullptr || SamplesNeeded <= 0)
    {
        return 0;
    }

    // Early-out on teardown / hard-stop so the audio engine's source
    // stops immediately. Without this, the wave keeps padding silence
    // through BeginDestroy / PIE shutdown, piling up audio-mixer
    // source-command backlog and freezing the editor on stop PIE.
    // Re-activated by AppendFloat32Frames when fresh data lands.
    if (!bActive.Load())
    {
        return 0;
    }

    const int32 Channels = FMath::Max(NumChannels, 1);
    const int32 FramesRequested  = SamplesNeeded / Channels;
    const int32 SamplesRequested = FramesRequested * Channels;
    if (FramesRequested <= 0)
    {
        return 0;
    }

    TArray<float> FloatSamples;
    int32 FramesRead = 0;
    bool bShouldBroadcastFinished = false;
    bool bEndOfStream = false;
    {
        FScopeLock Lock(&SharedPCM->Guard);

        FloatSamples.SetNumUninitialized(SamplesRequested);

        int32 OutCursor = 0;
        while (OutCursor < SamplesRequested)
        {
            const int64 FramesAvailable = SharedPCM->TotalFrames - PlayedFrames;
            const int32 FramesStillWanted = (SamplesRequested - OutCursor) / Channels;
            const int32 FramesThisChunk = FMath::Min<int32>(
                FramesStillWanted,
                static_cast<int32>(FMath::Max<int64>(FramesAvailable, 0)));

            if (FramesThisChunk > 0)
            {
                const int32 SamplesThisChunk = FramesThisChunk * Channels;
                const int64 StartIndex = PlayedFrames * Channels;
                FMemory::Memcpy(
                    FloatSamples.GetData() + OutCursor,
                    SharedPCM->Data.GetData() + StartIndex,
                    SamplesThisChunk * sizeof(float));
                PlayedFrames += FramesThisChunk;
                OutCursor    += SamplesThisChunk;
                FramesRead   += FramesThisChunk;
                continue;
            }

            // No frames available. Three branches:
            //   1. Looping — wrap PlayedFrames back to 0 and keep
            //      filling this same engine request. The wave plays
            //      seamlessly from the start.
            //   2. Stop-on-finish + exhausted — signal EOS, bail.
            //   3. Streaming, buffer empty — leave the rest of the
            //      output filled with silence below and bail.
            if (bLooping && SharedPCM->TotalFrames > 0)
            {
                PlayedFrames = 0;
                bPlaybackFinishedBroadcasted = false;
                continue;
            }

            if (bStopSoundOnPlaybackFinish
                && SharedPCM->TotalFrames > 0
                && PlayedFrames >= SharedPCM->TotalFrames)
            {
                bEndOfStream = true;
                if (!bPlaybackFinishedBroadcasted)
                {
                    bPlaybackFinishedBroadcasted = true;
                    bShouldBroadcastFinished     = true;
                }
            }
            break;
        }

        // Zero-fill any remaining tail (streaming underrun).
        if (OutCursor < SamplesRequested)
        {
            FMemory::Memset(
                FloatSamples.GetData() + OutCursor,
                0,
                (SamplesRequested - OutCursor) * sizeof(float));
        }
    }

    // Convert all SamplesRequested floats to int16 for the engine.
    int16* Out = reinterpret_cast<int16*>(OutPCMData);
    for (int32 i = 0; i < SamplesRequested; ++i)
    {
        const float Clamped = FMath::Clamp(FloatSamples[i], -1.0f, 1.0f);
        Out[i] = static_cast<int16>(Clamped * 32767.0f);
    }

    // Visualization broadcast. Skip when inactive / nobody listening
    // / we had zero real frames (avoids fake-amplitude broadcasts
    // during full-underrun silences).
    if (FramesRead > 0
        && bActive.Load()
        && (OnGeneratePCMData.IsBound() || OnGeneratePCMDataNative.IsBound()))
    {
        // Only the first FramesRead * Channels samples are real —
        // the rest was silence padding.
        const int32 RealSamples = FramesRead * Channels;

        if (NumSamplesPerChunk <= 0)
        {
            // No batching — one broadcast per engine poll.
            TArray<float> Batch;
            Batch.SetNumUninitialized(RealSamples);
            FMemory::Memcpy(Batch.GetData(), FloatSamples.GetData(), RealSamples * sizeof(float));

            TWeakObjectPtr<UInoAgentsImportedSoundWave> WeakThis(this);
            AsyncTask(ENamedThreads::GameThread,
                [WeakThis, Batch = MoveTemp(Batch)]() mutable
                {
                    UInoAgentsImportedSoundWave* Self = WeakThis.Get();
                    if (Self == nullptr || !Self->bActive.Load())
                    {
                        return;
                    }
                    Self->OnGeneratePCMDataNative.Broadcast(Batch);
                    Self->OnGeneratePCMData.Broadcast(Batch);
                });
        }
        else
        {
            // Fixed-size batching. Append the real samples to the
            // carry, then slice out as many complete batches as are
            // ready. Remainder stays in the carry for next call.
            VisualizationCarry.Append(FloatSamples.GetData(), RealSamples);

            const int32 BatchSize = NumSamplesPerChunk;
            while (VisualizationCarry.Num() >= BatchSize)
            {
                TArray<float> Batch;
                Batch.SetNumUninitialized(BatchSize);
                FMemory::Memcpy(Batch.GetData(), VisualizationCarry.GetData(),
                                BatchSize * sizeof(float));
                VisualizationCarry.RemoveAt(0, BatchSize, EAllowShrinking::No);

                TWeakObjectPtr<UInoAgentsImportedSoundWave> WeakThis(this);
                AsyncTask(ENamedThreads::GameThread,
                    [WeakThis, Batch = MoveTemp(Batch)]() mutable
                    {
                        UInoAgentsImportedSoundWave* Self = WeakThis.Get();
                        if (Self == nullptr || !Self->bActive.Load())
                        {
                            return;
                        }
                        Self->OnGeneratePCMDataNative.Broadcast(Batch);
                        Self->OnGeneratePCMData.Broadcast(Batch);
                    });
            }
        }
    }

    if (bShouldBroadcastFinished)
    {
        BroadcastPlaybackFinished();
    }

    return bEndOfStream ? 0 : SamplesRequested * static_cast<int32>(sizeof(int16));
}

// ---------------------------------------------------------------------------
// Finish broadcast helper
// ---------------------------------------------------------------------------

void UInoAgentsImportedSoundWave::BroadcastPlaybackFinished()
{
    TWeakObjectPtr<UInoAgentsImportedSoundWave> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread,
        [WeakThis]()
        {
            UInoAgentsImportedSoundWave* Self = WeakThis.Get();
            if (Self == nullptr)
            {
                return;
            }
            Self->OnAudioPlaybackFinishedNative.Broadcast();
            Self->OnAudioPlaybackFinished.Broadcast();
        });
}
