// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include "Templates/Atomic.h"
#include "Templates/SharedPointer.h"
#include "HAL/CriticalSection.h"

#include "Audio/InoAudioTypes.h"

#include "InoImportedSoundWave.generated.h"

/**
 * Shared PCM storage held via TSharedRef so DuplicateSoundWave can
 * share the same buffer across multiple sound-wave instances. Each
 * instance keeps its own playback cursor (PlayedFrames) but reads
 * from — and, for the streaming source, writes to — this shared
 * struct.
 *
 * Not a UObject — it's plain data with a companion lock.
 */
struct FInoSharedPCMBuffer
{
    /** Float32 interleaved PCM. Length = TotalFrames * NumChannels. */
    TArray64<float> Data;

    /** Complete frames present in Data. */
    int64 TotalFrames = 0;

    /** Serialises all reads/writes of Data + TotalFrames across
     *  audio render thread, worker threads, and game thread. Held
     *  briefly; never across an async dispatch. */
    mutable FCriticalSection Guard;
};

/**
 * Base procedural sound wave for the InoAgents runtime audio stack.
 *
 * Inherits USoundWaveProcedural but doesn't use its internal byte
 * queue — instead references a float32 PCM buffer (owned via
 * TSharedRef so duplicates can share storage) and fills the audio
 * engine's output via an overridden GeneratePCMData.
 *
 * Float32 storage is deliberate: samples flow through resampling /
 * channel-mixing / visualization paths without loss, and the
 * visualization delegates hand data out without per-call conversion.
 * Conversion to int16 happens once, at the audio-render-thread
 * boundary where the engine needs it.
 *
 * Threading:
 *   - GeneratePCMData runs on the audio render thread.
 *   - Append/Release paths run on a worker (via the subclass pipe).
 *   - SharedPCM->Guard serialises access across those threads.
 *   - Delegate broadcasts marshal to the game thread via AsyncTask.
 *   - bActive atomic gates the audio-thread visualization path; the
 *     game thread can short-circuit broadcasts instantly at teardown
 *     even though the audio engine's Stop() is asynchronous.
 */
UCLASS(BlueprintType, Category = "InoAgents|Audio")
class INOAGENTS_API UInoImportedSoundWave : public USoundWaveProcedural
{
    GENERATED_BODY()

public:
    UInoImportedSoundWave(const FObjectInitializer& ObjectInitializer);

    //~ USoundWaveProcedural interface
    virtual int32 GeneratePCMData(uint8* OutPCMData, const int32 SamplesNeeded) override;
    virtual void  BeginDestroy() override;
    //~ End USoundWaveProcedural interface

    //~ USoundWave interface
    /**
     * Per-tick parse, called by the audio engine on the audio thread
     * for each FActiveSound playing this wave. We override to ACTIVELY
     * stop the active sound when:
     *   1. bActive has been flipped false (force-stop / teardown), or
     *   2. The buffer has fully drained AND
     *      bStopSoundOnPlaybackFinish is true.
     *
     * The active stop is what cleanly drains the audio mixer's source-
     * command queue on PIE shutdown — without it, returning 0 from
     * GeneratePCMData alone leaves the source in limbo and the mixer
     * times out waiting to flush its commands.
     *
     * Pattern lifted from RuntimeAudio's UImportedSoundWave::Parse.
     */
    virtual void Parse(class FAudioDevice* AudioDevice,
                       const UPTRINT NodeWaveInstanceHash,
                       struct FActiveSound& ActiveSound,
                       const struct FSoundParseParameters& ParseParams,
                       TArray<struct FWaveInstance*>& WaveInstances) override;
    //~ End USoundWave interface

    // =================================================================
    // Factory
    // =================================================================

    /** Allocate a new imported sound wave. Game thread only. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    static UInoImportedSoundWave* CreateImportedSoundWave();

    // =================================================================
    // Playback state
    // =================================================================

    /** Current playback position in seconds. Thread-safe. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio|Info")
    float GetPlaybackTime() const;

    /** Total duration of the currently-buffered audio, in seconds.
     *  Grows as new data is appended. Thread-safe. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio|Info")
    float GetDurationSeconds() const;

    /** Frames consumed by playback so far. Thread-safe. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio|Info")
    int64 GetPlayedFrames() const;

    /** Total frames in the shared buffer (played + unplayed). */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio|Info")
    int64 GetTotalFrames() const;

    /** Playback progress as 0..100. Returns 0 for an empty buffer. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio|Info")
    float GetPlaybackPercentage() const;

    /** True when PlayedFrames has reached TotalFrames. Use the
     *  OnAudioPlaybackFinished delegate for an event-driven signal;
     *  use this for poll-style Blueprint graphs. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio|Info")
    bool IsPlaybackFinished() const;

    /** Configured sample rate in Hz. Thread-safe (cached on the wave). */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio|Info")
    int32 GetSampleRate() const;

    /** Channel count (1 = mono, 2 = stereo). Thread-safe. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio|Info")
    int32 GetNumOfChannels() const;

    /** Single-call metadata snapshot. Cheaper than four separate
     *  queries since it takes the lock once. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio|Info")
    FInoAudioHeaderInfo GetAudioHeaderInfo() const;

    // =================================================================
    // Playback control
    // =================================================================

    /** Reset playback position to StartTime seconds. Clamped to
     *  [0, duration]. Thread-safe. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Control")
    void RewindPlaybackTime(float StartTime);

    /**
     * Drop already-played frames from the buffer and rebase the
     * playback position. Useful for long-running streams (e.g. mic
     * capture) where unbounded growth would otherwise leak memory.
     * Not safe when the buffer is shared with a duplicate — the
     * duplicate would see its cursor silently advance past data that
     * was removed underneath it. Thread-safe w.r.t. access, but
     * callers are responsible for not sharing at release time.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Control")
    void ReleasePlayedAudioData();

    /**
     * Drop every frame in the buffer and reset the playback cursor.
     * Equivalent to starting over with an empty wave. Unlike
     * ReleasePlayedAudioData this also drops unplayed samples —
     * useful for a clean "reset before next stream" between unrelated
     * audio sessions.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Control")
    void ReleaseMemory();

    /**
     * When true, playback wraps from end-of-buffer back to frame 0
     * instead of draining. Combine with SetStopSoundOnPlaybackFinish=
     * false (the streaming default) to get an infinite loop.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Control")
    void SetSoundLooping(bool bLoop);

    /**
     * Hard-stop playback: close the visualization gate, clear the
     * buffer, and force the next GeneratePCMData poll to return 0 so
     * the audio engine's source stops immediately. Use this on
     * teardown or when a hard "remove the audio" semantic is needed
     * without waiting for the wave to drain naturally.
     *
     * After this, the wave is still valid — new data can be appended
     * and the source re-Played. bActive is re-enabled on the next
     * AppendFloat32Frames call.
     *
     * Thread-safe.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Control")
    void ForceStopPlayback();

    /** Current loop flag. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio|Control")
    bool IsSoundLooping() const;

    // =================================================================
    // Format
    // =================================================================

    /** Set the wave's sample rate. Call BEFORE the first append;
     *  changing the rate mid-stream is not supported. See
     *  ResampleSoundWave for post-population rate changes. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Format")
    void SetInitialDesiredSampleRate(int32 InSampleRate);

    /** Set channel count (1 = mono, 2 = stereo). Same pre-first-append
     *  constraint as SetInitialDesiredSampleRate. See
     *  MixSoundWaveChannels for post-population changes. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Format")
    void SetInitialDesiredNumChannels(int32 InNumChannels);

    // =================================================================
    // Post-population transforms
    // =================================================================

    /**
     * Resample the entire buffer to NewSampleRate. Linear
     * interpolation — fine for voice, audible on musical content.
     * Playback cursor is rescaled to preserve relative position.
     * Returns false if NewSampleRate is invalid or the buffer is
     * empty. Thread-safe against playback; holds the lock for the
     * duration of the resample.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Transform")
    bool ResampleSoundWave(int32 NewSampleRate);

    /**
     * Change the wave's channel count. 1→2 duplicates the mono sample
     * into both channels; 2→1 averages the two channels.
     * Other conversions are rejected (we don't do surround). Cursor
     * preserved. Thread-safe.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Transform")
    bool MixSoundWaveChannels(int32 NewNumChannels);

    /**
     * Reverse the PCM buffer in-place for backward playback. Playback
     * cursor is mirrored — if you were halfway through, you stay
     * halfway through but now playing toward the new "end" (original
     * "start"). Thread-safe.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Transform")
    void ReverseAudioBuffer();

    // =================================================================
    // Buffer access
    // =================================================================

    /**
     * Copy the entire PCM buffer out as interleaved float32. Held
     * under the data lock for the duration of the copy. Returns a
     * 32-bit TArray; if the stream has grown past 2 GB of float
     * samples (unusual), the result is truncated at MAX_int32.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Buffer")
    TArray<float> GetPCMBuffer() const;

    /** Non-Blueprint const& accessor into the shared buffer. Cheap
     *  (no copy). Caller must hold a read on the returned storage
     *  only as long as DataGuard is held — use
     *  AcquirePCMBufferLock() / ReleasePCMBufferLock() if manually
     *  locking, or just call GetPCMBuffer() for safe copy semantics. */
    const TArray64<float>& GetPCMBufferRef() const { return SharedPCM->Data; }

    /** Acquire/release the shared buffer's lock — exposed so
     *  advanced C++ consumers can do zero-copy reads. Always pair
     *  one Acquire with one Release. Prefer GetPCMBuffer() for the
     *  typical case. */
    void AcquirePCMBufferLock() const { SharedPCM->Guard.Lock(); }
    void ReleasePCMBufferLock() const { SharedPCM->Guard.Unlock(); }

    // =================================================================
    // Duplication (parallel playback)
    // =================================================================

    /**
     * Create a copy of this sound wave. When bShareBuffer is true,
     * both waves reference the SAME underlying PCM storage — each
     * has its own playback cursor, so you can play the same audio
     * at two different positions (e.g. two characters hearing the
     * same recording at different times) without doubling memory.
     *
     * When bShareBuffer is false, the new wave gets its own copy of
     * the PCM data — independent playback and independent lifetime,
     * at the cost of 2× memory for the audio payload.
     *
     * Returns the new wave, or nullptr if duplication failed.
     * Game thread only.
     *
     * Warning: ReleasePlayedAudioData and ReleaseMemory are
     * destructive to shared storage — don't call them on a wave
     * that still has live duplicates sharing its buffer.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Duplicate")
    UInoImportedSoundWave* DuplicateSoundWave(bool bShareBuffer);

    // =================================================================
    // Visualization batching
    // =================================================================

    /**
     * Number of interleaved samples per OnGeneratePCMData broadcast.
     * When > 0, GeneratePCMData accumulates emitted samples in an
     * internal carry buffer and broadcasts fixed-size chunks — useful
     * for lip-sync / viseme consumers that need a predictable cadence
     * (e.g. 160 samples at 16 kHz = 10 ms per broadcast). When 0
     * (default), each engine poll produces one broadcast of whatever
     * size the engine requested (typically 1024 samples).
     *
     * Counts INTERLEAVED samples — so 160 samples at 2 channels =
     * 80 frames = 5 ms at 16 kHz.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Visualization")
    void SetNumSamplesPerChunk(int32 NumSamples);

    /** Current visualization batch size. 0 = one batch per engine poll. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio|Visualization")
    int32 GetNumSamplesPerChunk() const { return NumSamplesPerChunk; }

    // =================================================================
    // Delegates
    // =================================================================

    /** C++-only native variant of OnGeneratePCMData. */
    FOnInoGeneratePCMDataNative OnGeneratePCMDataNative;

    /** Fires on the game thread during playback with interleaved
     *  float samples the audio engine is consuming. Batch size
     *  controlled by SetNumSamplesPerChunk. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoGeneratePCMData OnGeneratePCMData;

    /** C++-only native variant of OnPopulateAudioData. */
    FOnInoPopulateAudioDataNative OnPopulateAudioDataNative;

    /** Fires on the game thread when new PCM frames are appended
     *  to the buffer. Payload is the newly-appended samples only. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoPopulateAudioData OnPopulateAudioData;

    /** C++-only native variant of OnPopulateAudioState. */
    FOnInoPopulateAudioStateNative OnPopulateAudioStateNative;

    /** Lightweight companion to OnPopulateAudioData — same trigger,
     *  no payload. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoPopulateAudioState OnPopulateAudioState;

    /** C++-only native variant of OnAudioPlaybackFinished. */
    FOnInoAudioPlaybackFinishedNative OnAudioPlaybackFinishedNative;

    /** Fires once when the PCM buffer has been fully played through.
     *  Only fires when the subclass has opted in via
     *  SetStopSoundOnPlaybackFinish(true). */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoAudioPlaybackFinished OnAudioPlaybackFinished;

    /** C++-only native variant of OnAudioError. */
    FOnInoAudioErrorNative OnAudioErrorNative;

    /** Fires on the game thread on any non-recoverable error. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoAudioError OnAudioError;

protected:
    // =================================================================
    // Shared PCM storage
    // =================================================================

    /**
     * PCM data — held as a TSharedRef so duplicate instances can
     * share the same backing storage. Constructor allocates a fresh
     * instance; DuplicateSoundWave(bShareBuffer=true) copies this
     * ref into the new wave so both point at the same struct.
     */
    TSharedRef<FInoSharedPCMBuffer> SharedPCM =
        MakeShared<FInoSharedPCMBuffer>();

    /** Per-instance playback cursor. Never shared, even across
     *  duplicates — each sharer plays independently. */
    int64 PlayedFrames = 0;

    /**
     * If true, OnAudioPlaybackFinished fires once when PlayedFrames
     * reaches TotalFrames. If false (default), the wave returns
     * silence when the buffer runs dry — the streaming model.
     */
    bool bStopSoundOnPlaybackFinish = false;

    /** Latched after the first finished broadcast to prevent
     *  repeat fires. Reset on Rewind / Release / Append / Reverse /
     *  loop-wrap. */
    bool bPlaybackFinishedBroadcasted = false;

    // Loop flag is the inherited USoundWave::bLooping bitfield — we
    // don't declare our own to avoid shadowing (MSVC C4458). Read
    // and write via that member under SharedPCM->Guard.

    /**
     * Gate for the audio-thread visualization broadcast path. Set
     * false in BeginDestroy; subclasses may flip it during teardown
     * to stop broadcasts the instant a reset happens.
     */
    TAtomic<bool> bActive{true};

    // =================================================================
    // Visualization batching state
    // =================================================================

    /** Target sample count per OnGeneratePCMData broadcast; 0 = one
     *  broadcast per engine poll. Set via SetNumSamplesPerChunk. */
    int32 NumSamplesPerChunk = 0;

    /** Protects VisualizationCarry — mutated from the audio thread in
     *  GeneratePCMData and from the game thread in BeginDestroy /
     *  ForceStopPlayback. Without this lock, concurrent Append +
     *  Reset on the same TArray races at the heap level and was
     *  previously locking up the audio mixer's source-command queue
     *  on PIE shutdown. Held very briefly, never across an async
     *  dispatch. */
    mutable FCriticalSection VisualizationGuard;

    /** Accumulator for samples that haven't yet filled a full
     *  NumSamplesPerChunk batch. Carried across GeneratePCMData calls.
     *  Only touched on the audio render thread. */
    TArray<float> VisualizationCarry;

    // =================================================================
    // Helpers callable from subclasses
    // =================================================================

    /**
     * Append a block of float32 interleaved samples to the shared
     * buffer. Acquires SharedPCM->Guard itself — safe to call from
     * any thread. Broadcasts OnPopulateAudioData + OnPopulateAudioState
     * to the game thread.
     *
     * NumFramesInBlock must equal (Interleaved.Num() / NumChannels).
     */
    void AppendFloat32Frames(TArray<float>&& Interleaved, int64 NumFramesInBlock);

    /** Broadcasts to OnAudioPlaybackFinished / its native sibling on
     *  the game thread. Safe to call from the audio thread. */
    void BroadcastPlaybackFinished();

    /** Flush any bytes stuck in VisualizationCarry. Called by
     *  BeginDestroy + state-reset paths so we don't leak a stale
     *  partial batch across streams. Audio thread only. */
    void ResetVisualizationCarry();
};
