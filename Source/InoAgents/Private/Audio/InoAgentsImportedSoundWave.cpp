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

    Super::BeginDestroy();
}

// ---------------------------------------------------------------------------
// Format
// ---------------------------------------------------------------------------

void UInoAgentsImportedSoundWave::SetInitialDesiredSampleRate(int32 InSampleRate)
{
    // SetSampleRate on USoundWaveProcedural takes uint32; clamp to a
    // sane range to avoid accidental 0 / negative values sneaking
    // into the audio engine.
    const uint32 Clamped = static_cast<uint32>(FMath::Clamp(InSampleRate, 8000, 192000));
    SetSampleRate(Clamped);
}

void UInoAgentsImportedSoundWave::SetInitialDesiredNumChannels(int32 InNumChannels)
{
    NumChannels = FMath::Clamp(InNumChannels, 1, 2);
}

// ---------------------------------------------------------------------------
// Playback state queries
// ---------------------------------------------------------------------------

float UInoAgentsImportedSoundWave::GetPlaybackTime() const
{
    const int32 Rate = FMath::Max<int32>(static_cast<int32>(GetSampleRateForCurrentPlatform()), 1);
    FScopeLock Lock(&DataGuard);
    return static_cast<float>(PlayedFrames) / static_cast<float>(Rate);
}

float UInoAgentsImportedSoundWave::GetDurationSeconds() const
{
    const int32 Rate = FMath::Max<int32>(static_cast<int32>(GetSampleRateForCurrentPlatform()), 1);
    FScopeLock Lock(&DataGuard);
    return static_cast<float>(TotalFrames) / static_cast<float>(Rate);
}

int64 UInoAgentsImportedSoundWave::GetPlayedFrames() const
{
    FScopeLock Lock(&DataGuard);
    return PlayedFrames;
}

int64 UInoAgentsImportedSoundWave::GetTotalFrames() const
{
    FScopeLock Lock(&DataGuard);
    return TotalFrames;
}

void UInoAgentsImportedSoundWave::RewindPlaybackTime(float StartTime)
{
    const int32 Rate = FMath::Max<int32>(static_cast<int32>(GetSampleRateForCurrentPlatform()), 1);
    const int64 TargetFrame = FMath::Max<int64>(0, static_cast<int64>(StartTime * Rate));

    FScopeLock Lock(&DataGuard);
    PlayedFrames = FMath::Min<int64>(TargetFrame, TotalFrames);
    // Rewinding un-latches the finished broadcast so playback can
    // complete again after seeking back.
    bPlaybackFinishedBroadcasted = false;
}

void UInoAgentsImportedSoundWave::ReleasePlayedAudioData()
{
    FScopeLock Lock(&DataGuard);

    if (PlayedFrames <= 0 || TotalFrames <= 0)
    {
        return;
    }

    const int32 Channels = FMath::Max(NumChannels, 1);
    const int64 SamplesToDrop = PlayedFrames * Channels;

    // TArray64::RemoveAt shifts trailing elements forward — O(N) but
    // N here is only the unplayed tail, which is small for any
    // realistic call site (periodic trim of a live capture stream).
    if (SamplesToDrop > 0 && SamplesToDrop <= PCMBuffer.Num())
    {
        PCMBuffer.RemoveAt(0, SamplesToDrop, EAllowShrinking::No);
    }

    TotalFrames -= PlayedFrames;
    PlayedFrames = 0;
    bPlaybackFinishedBroadcasted = false;
}

// ---------------------------------------------------------------------------
// Buffer access
// ---------------------------------------------------------------------------

TArray<float> UInoAgentsImportedSoundWave::GetPCMBuffer() const
{
    FScopeLock Lock(&DataGuard);

    // Return a TArray<float> (int32-indexed). Streams that have grown
    // past 2 GB of float samples are beyond Blueprint's reach anyway;
    // we clamp to int32 max to avoid a hard crash in that case.
    const int64 Count = FMath::Min<int64>(PCMBuffer.Num(), MAX_int32);
    TArray<float> Out;
    Out.SetNumUninitialized(static_cast<int32>(Count));
    if (Count > 0)
    {
        FMemory::Memcpy(Out.GetData(), PCMBuffer.GetData(), Count * sizeof(float));
    }
    return Out;
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

    // Move the new data into the buffer under lock. Populate delegates
    // dispatch a copy outside the lock so listeners never block the
    // append path.
    TArray<float> BroadcastCopy;
    const bool bWantPopulateData =
        OnPopulateAudioData.IsBound() || OnPopulateAudioDataNative.IsBound();
    if (bWantPopulateData)
    {
        BroadcastCopy = Interleaved;
    }

    {
        FScopeLock Lock(&DataGuard);
        PCMBuffer.Append(MoveTemp(Interleaved));
        TotalFrames += NumFramesInBlock;
        // New data invalidates a latched "playback finished" — the
        // wave is no longer exhausted, so future playback can fire
        // the finished broadcast again.
        bPlaybackFinishedBroadcasted = false;
    }

    // Marshal the broadcasts to the game thread. Capture a weak ptr
    // so a GC during flight doesn't UAF the delegates.
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
    // The audio engine hands us a byte buffer sized for SamplesNeeded
    // interleaved int16 samples (= SamplesNeeded * sizeof(int16) bytes).
    // For stereo, "SamplesNeeded" counts both channels' samples — e.g.
    // at 48 kHz stereo, ~2400 samples per callback = 1200 frames.
    //
    // Return-value contract (from USoundWaveProcedural):
    //   > 0  — bytes filled into the output buffer. The source keeps
    //          polling for more.
    //   = 0  — end of stream. The source stops, the UAudioComponent
    //          reports IsPlaying() == false.
    //
    // For streaming we want the source to stay alive through buffer
    // underruns, so we pad the output with silence and return a
    // non-zero byte count. Only when bStopSoundOnPlaybackFinish is
    // true AND the buffer is fully played do we return 0 — the "we
    // really are done" signal.

    if (OutPCMData == nullptr || SamplesNeeded <= 0)
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

    // Copy out the float samples under lock, then release the lock
    // before doing the int16 conversion + broadcast dispatch. Holding
    // the lock across the conversion loop would serialise against
    // appends from worker threads for no good reason.
    TArray<float> FloatSamples;
    int32 FramesToRead = 0;
    bool bShouldBroadcastFinished = false;
    bool bEndOfStream = false;
    {
        FScopeLock Lock(&DataGuard);

        const int64 FramesAvailable = TotalFrames - PlayedFrames;
        FramesToRead = FMath::Min<int32>(
            FramesRequested,
            static_cast<int32>(FMath::Max<int64>(FramesAvailable, 0)));

        if (FramesToRead > 0)
        {
            const int32 SamplesToCopy = FramesToRead * Channels;
            const int64 StartIndex    = PlayedFrames * Channels;

            FloatSamples.SetNumUninitialized(SamplesToCopy);
            FMemory::Memcpy(
                FloatSamples.GetData(),
                PCMBuffer.GetData() + StartIndex,
                SamplesToCopy * sizeof(float));

            PlayedFrames += FramesToRead;
        }

        // If the buffer has exhausted and we're configured to stop on
        // finish, flag a one-shot broadcast AND signal end-of-stream
        // back to the engine. We latch the flag here so repeated polls
        // after exhaustion don't re-fire the delegate, but we still
        // return 0 each time so the engine knows to stop the source.
        if (bStopSoundOnPlaybackFinish
            && TotalFrames > 0
            && PlayedFrames >= TotalFrames)
        {
            bEndOfStream = true;
            if (!bPlaybackFinishedBroadcasted)
            {
                bPlaybackFinishedBroadcasted = true;
                bShouldBroadcastFinished     = true;
            }
        }
    }

    int16* Out = reinterpret_cast<int16*>(OutPCMData);
    const int32 SamplesWritten = FramesToRead * Channels;

    // Convert whatever real samples we had to int16.
    for (int32 i = 0; i < SamplesWritten; ++i)
    {
        const float Clamped = FMath::Clamp(FloatSamples[i], -1.0f, 1.0f);
        Out[i] = static_cast<int16>(Clamped * 32767.0f);
    }

    // Pad any shortfall with silence so the engine stays happy during
    // streaming underruns. Zero int16 samples = silence.
    if (SamplesWritten < SamplesRequested)
    {
        FMemory::Memset(
            Out + SamplesWritten,
            0,
            (SamplesRequested - SamplesWritten) * sizeof(int16));
    }

    // Hand the float copy of whatever we actually had to the game
    // thread for visualization. Skip the dispatch when nobody is
    // listening OR when the visualization gate is closed (teardown)
    // OR when we had nothing real to visualise this round (avoids
    // fake zero-amplitude broadcasts during underruns).
    if (FramesToRead > 0
        && bActive.Load()
        && (OnGeneratePCMData.IsBound() || OnGeneratePCMDataNative.IsBound()))
    {
        TWeakObjectPtr<UInoAgentsImportedSoundWave> WeakThis(this);
        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, Batch = MoveTemp(FloatSamples)]() mutable
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

    if (bShouldBroadcastFinished)
    {
        BroadcastPlaybackFinished();
    }

    // Return 0 bytes when we're actively signalling end-of-stream —
    // that's how USoundWaveProcedural tells the audio source to stop.
    // Otherwise return the full requested byte count (real + silence
    // padding) so the source keeps polling us for more.
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
