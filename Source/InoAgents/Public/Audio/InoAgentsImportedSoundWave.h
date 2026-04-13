// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include "Templates/Atomic.h"
#include "HAL/CriticalSection.h"

#include "Audio/InoAgentsAudioTypes.h"

#include "InoAgentsImportedSoundWave.generated.h"

/**
 * Base procedural sound wave for the InoAgents runtime audio stack.
 *
 * Inherits USoundWaveProcedural but does not use its internal byte
 * queue — instead owns its own float32 PCM buffer and fills the audio
 * engine's output via an overridden GeneratePCMData.
 *
 * Float32 storage is a deliberate choice: samples flow through
 * resampling / channel-mixing / visualization paths lossless, and the
 * visualization delegates (OnGeneratePCMData, OnPopulateAudioData) can
 * hand the data out without a per-call conversion. Conversion to int16
 * happens once, at the audio-render-thread boundary, where the engine
 * actually needs it.
 *
 * Not meant to be instantiated directly — subclass for a specific data
 * source (UInoAgentsStreamingSoundWave for fed bytes,
 * UInoAgentsCapturableSoundWave for microphone input). The base class
 * is concrete only so it can serve as a common typed pointer for
 * delegates and polymorphic access.
 *
 * Threading:
 *   - GeneratePCMData runs on the audio render thread.
 *   - Append/Release paths run on a worker (via the subclass' task pipe).
 *   - DataGuard serialises access to PCMBuffer / TotalFrames /
 *     PlayedFrames between those threads.
 *   - Every delegate broadcast is marshaled to the game thread via
 *     AsyncTask(ENamedThreads::GameThread, ...).
 *   - bActive is an atomic gate so the audio thread can short-circuit
 *     its visualization path the instant the game thread calls for a
 *     reset — the audio engine may continue calling GeneratePCMData
 *     for a frame or two after Stop() because the stop itself is
 *     asynchronous.
 */
UCLASS(BlueprintType, Category = "InoAgents|Audio")
class INOAGENTS_API UInoAgentsImportedSoundWave : public USoundWaveProcedural
{
    GENERATED_BODY()

public:
    UInoAgentsImportedSoundWave(const FObjectInitializer& ObjectInitializer);

    //~ USoundWaveProcedural interface
    virtual int32 GeneratePCMData(uint8* OutPCMData, const int32 SamplesNeeded) override;
    virtual void  BeginDestroy() override;
    //~ End USoundWaveProcedural interface

    // =================================================================
    // Playback state
    // =================================================================

    /** Current playback position in seconds. 0 at the start of the
     *  buffer; advances as the audio engine pulls samples. Thread-safe. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio")
    float GetPlaybackTime() const;

    /** Total duration of the currently-buffered audio, in seconds.
     *  Grows as new data is appended. Thread-safe. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio")
    float GetDurationSeconds() const;

    /** Number of frames played so far. Thread-safe. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio")
    int64 GetPlayedFrames() const;

    /** Total frames currently in the buffer (played + unplayed).
     *  Thread-safe. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio")
    int64 GetTotalFrames() const;

    /** Reset playback position to StartTime seconds. Clamped to
     *  [0, duration]. Thread-safe. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    void RewindPlaybackTime(float StartTime);

    /**
     * Drop already-played frames from the buffer and rebase the
     * playback position. Useful for long-running streams (e.g. mic
     * capture) where unbounded growth would otherwise leak memory.
     *
     * No-op if nothing has played yet. Thread-safe.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    void ReleasePlayedAudioData();

    // =================================================================
    // Format
    // =================================================================

    /**
     * Set the wave's sample rate. Call BEFORE the first append; changing
     * the rate mid-stream is not supported (USoundWaveProcedural caches
     * the rate at first playback and ignores later changes). Thread-safe
     * with respect to PCM access but should be serialised with Append.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Format")
    void SetInitialDesiredSampleRate(int32 InSampleRate);

    /**
     * Set the wave's channel count (1 = mono, 2 = stereo). Same
     * pre-first-append constraint as SetInitialDesiredSampleRate.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Format")
    void SetInitialDesiredNumChannels(int32 InNumChannels);

    // =================================================================
    // Buffer access
    // =================================================================

    /**
     * Copy the entire PCM buffer out as interleaved float32. Held under
     * the data lock for the duration of the copy — keep the buffer
     * small or call from a worker thread to avoid stalling playback.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Buffer")
    TArray<float> GetPCMBuffer() const;

    // =================================================================
    // Delegates
    // =================================================================

    /** C++-only native variant of OnGeneratePCMData. Bind via AddRaw /
     *  AddLambda for lower overhead than the dynamic multicast. */
    FOnInoAgentsGeneratePCMDataNative OnGeneratePCMDataNative;

    /** Fires on the game thread during playback with the interleaved
     *  float samples the audio engine just consumed. Payload includes
     *  every channel for the frames covered by the current audio-engine
     *  pull (typically 10-25 ms of audio). Use for waveform display,
     *  lip-sync, VU metering. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoAgentsGeneratePCMData OnGeneratePCMData;

    /** C++-only native variant of OnPopulateAudioData. */
    FOnInoAgentsPopulateAudioDataNative OnPopulateAudioDataNative;

    /** Fires on the game thread whenever new PCM frames are appended
     *  to the buffer. Payload is the newly-appended samples only
     *  (not the full buffer). Use for real-time analysis of incoming
     *  audio before it reaches playback. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoAgentsPopulateAudioData OnPopulateAudioData;

    /** C++-only native variant of OnPopulateAudioState. */
    FOnInoAgentsPopulateAudioStateNative OnPopulateAudioStateNative;

    /** Lightweight companion to OnPopulateAudioData — same trigger, no
     *  payload. Listeners that want to poll the buffer themselves can
     *  bind here to avoid the per-append float array copy. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoAgentsPopulateAudioState OnPopulateAudioState;

    /** C++-only native variant of OnAudioPlaybackFinished. */
    FOnInoAgentsAudioPlaybackFinishedNative OnAudioPlaybackFinishedNative;

    /** Fires once when the PCM buffer has been fully played through.
     *  Only fires when the subclass has opted in via
     *  SetStopSoundOnPlaybackFinish(true); otherwise the wave keeps
     *  playing silence, waiting for more data. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoAgentsAudioPlaybackFinished OnAudioPlaybackFinished;

    /** C++-only native variant of OnAudioError. */
    FOnInoAgentsAudioErrorNative OnAudioErrorNative;

    /** Fires on the game thread on any non-recoverable error
     *  (decoder failure, capture open failure, etc.). The wave
     *  itself stays valid — caller decides whether to retry. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoAgentsAudioError OnAudioError;

protected:
    // =================================================================
    // Shared state (subclasses access under DataGuard)
    // =================================================================

    /**
     * Float32 PCM buffer, interleaved. One entry per (frame * channel),
     * so for stereo the array is 2 * TotalFrames long. Protected by
     * DataGuard. TArray64 because indefinite-length streams can exceed
     * the 2 GB int32 cap for float samples at high sample rates.
     */
    TArray64<float> PCMBuffer;

    /** Complete frames in PCMBuffer. Protected by DataGuard. */
    int64 TotalFrames = 0;

    /** Frames consumed by GeneratePCMData so far. Protected by DataGuard. */
    int64 PlayedFrames = 0;

    /**
     * If true, OnAudioPlaybackFinished fires once when PlayedFrames
     * reaches TotalFrames. If false (default), the wave returns
     * silence when the buffer runs dry and keeps returning silence
     * until more data arrives — the streaming model for live TTS /
     * ongoing capture where "buffer empty" isn't "stream ended".
     *
     * Set by UInoAgentsStreamingSoundWave::SetStopSoundOnPlaybackFinish.
     */
    bool bStopSoundOnPlaybackFinish = false;

    /** Latched after the first OnAudioPlaybackFinished broadcast to
     *  prevent repeat fires if the audio engine keeps polling after
     *  the buffer drains. Reset by Rewind / Release paths. */
    bool bPlaybackFinishedBroadcasted = false;

    /** Serialises access to PCMBuffer / TotalFrames / PlayedFrames
     *  across the audio render thread, worker threads, and the game
     *  thread. Held briefly — never across an async dispatch. */
    mutable FCriticalSection DataGuard;

    /**
     * Gate for the audio-thread visualization broadcast path. Set
     * false in BeginDestroy; subclasses may flip it during teardown
     * to stop broadcasts the instant a reset happens, without waiting
     * for the audio engine to process Stop().
     */
    TAtomic<bool> bActive{true};

    // =================================================================
    // Helpers callable from subclasses
    // =================================================================

    /**
     * Append a block of float32 interleaved samples to the buffer.
     * Caller must hold DataGuard? NO — this helper acquires it itself.
     * Safe to call from any thread. Broadcasts OnPopulateAudioData +
     * OnPopulateAudioState to the game thread.
     *
     * NumFramesInBlock must equal (Interleaved.Num() / NumChannels).
     */
    void AppendFloat32Frames(TArray<float>&& Interleaved, int64 NumFramesInBlock);

private:
    /** Broadcasts to OnAudioPlaybackFinished / its native sibling.
     *  Marshals to game thread; safe to call from audio thread. */
    void BroadcastPlaybackFinished();
};
