// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Templates/PimplPtr.h"
#include "Tasks/Pipe.h"

#include "Audio/InoImportedSoundWave.h"
#include "Audio/InoAudioTypes.h"

#include "InoStreamingSoundWave.generated.h"

class FInoMp3DecodeState;

/**
 * Streaming procedural sound wave.
 *
 * Extends UInoImportedSoundWave with runtime append APIs: fed
 * bytes land in the PCM buffer as decoded float32 frames via a
 * background task pipe, so the game thread never pays for MP3
 * decoding or RAW-to-float transcoding.
 *
 * Supported append sources:
 *   - RAW PCM (Int16, Int32, UInt8, Float32) — AppendAudioDataFromRAW
 *   - MP3 bytes — AppendAudioDataFromMP3
 *
 * Typical lifecycle:
 *   Wave = UInoStreamingSoundWave::CreateStreamingSoundWave();
 *   Wave->SetInitialDesiredSampleRate(16000);
 *   Wave->SetInitialDesiredNumChannels(1);
 *   AudioComp->SetSound(Wave);
 *   AudioComp->Play();
 *   // ...later, from any thread:
 *   Wave->AppendAudioDataFromRAW(ChunkBytes, EInoRawAudioFormat::Int16, 16000, 1);
 *   // ...eventually:
 *   Wave->SetStopSoundOnPlaybackFinish(true); // fire OnAudioPlaybackFinished on drain
 */
UCLASS(BlueprintType, Category = "InoAgents|Audio")
class INOAGENTS_API UInoStreamingSoundWave : public UInoImportedSoundWave
{
    GENERATED_BODY()

public:
    UInoStreamingSoundWave(const FObjectInitializer& ObjectInitializer);

    //~ UObject interface
    virtual void BeginDestroy() override;
    //~ End UObject interface

    // =================================================================
    // Factory
    // =================================================================

    /** Allocate a new streaming wave. Prefer over NewObject so the
     *  wave is guaranteed to start with a clean task pipe. Must be
     *  called from the game thread. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Streaming")
    static UInoStreamingSoundWave* CreateStreamingSoundWave();

    // =================================================================
    // Append APIs
    // =================================================================

    /**
     * Append a buffer of RAW PCM bytes. Bytes are transcoded to
     * float32 on a worker thread, then appended to the wave's
     * playback buffer in the order this function was called.
     *
     * @param RAWData       The PCM bytes. Must be a whole number of
     *                      samples in the chosen format (incomplete
     *                      trailing samples are dropped).
     * @param RAWFormat     How each sample is encoded.
     * @param InSampleRate  Source sample rate. Must match the wave's
     *                      configured rate; mismatched rates are
     *                      rejected with an error broadcast (v1 has
     *                      no resampling — upstream pipelines must
     *                      resample before calling).
     * @param InNumChannels Source channel count. Same constraint as
     *                      sample rate.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Streaming")
    void AppendAudioDataFromRAW(
        const TArray<uint8>&       RAWData,
        EInoRawAudioFormat   RAWFormat,
        int32                      InSampleRate,
        int32                      InNumChannels);

    /**
     * Append a buffer of MP3 bytes. Bytes feed into a rolling decoder
     * on a worker thread — partial frames are retained between calls.
     * Format (sample rate, channels) is auto-detected from the first
     * decoded frame; subsequent calls inherit that format.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Streaming")
    void AppendAudioDataFromMP3(const TArray<uint8>& MP3Data);

    /**
     * Reserve capacity in the PCM buffer for NumOfBytesToPreAllocate
     * future frames — avoids reallocations mid-stream for large
     * known-size streams. Result callback fires on the game thread.
     *
     * NumOfBytesToPreAllocate is measured in bytes of decoded
     * float32 data; 1 second of 44.1 kHz mono = 176400 bytes,
     * stereo = 352800 bytes.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Streaming")
    void PreAllocateAudioData(
        int64 NumOfBytesToPreAllocate,
        const FOnInoPreAllocateAudioDataResult& Result);

    /** Native (C++) variant of PreAllocateAudioData. */
    void PreAllocateAudioData(
        int64 NumOfBytesToPreAllocate,
        const FOnInoPreAllocateAudioDataResultNative& Result);

    /**
     * When true, OnAudioPlaybackFinished fires as soon as the buffer
     * drains and the wave returns empty PCM to the audio engine (the
     * engine will stop shortly after). When false (default), the
     * wave returns silence on drain and waits for more data — the
     * streaming model for live TTS or ongoing mic capture where
     * "empty buffer" isn't "stream ended".
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Streaming")
    void SetStopSoundOnPlaybackFinish(bool bStop);

    /** Drop all currently-buffered samples and reset playback. Useful
     *  for "abort this stream" — the wave stays usable for a new
     *  stream without needing to allocate a fresh one. Thread-safe. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio|Streaming")
    void ResetStreamingBuffer();

protected:
    /**
     * Serialises append / preallocate tasks so their side effects on
     * PCMBuffer land in call order even when callers fire from
     * multiple threads. Allocated in the constructor; nulled in
     * BeginDestroy after the pipe has drained.
     */
    TUniquePtr<UE::Tasks::FPipe> AudioTaskPipe;

    /**
     * Rolling MP3 decoder state. Allocated lazily on first MP3 feed,
     * held across Feed calls so partial frames persist. Accessed
     * only from within the task pipe — no external locking needed.
     */
    TPimplPtr<FInoMp3DecodeState> Mp3State;

    /**
     * Sample rate / channel count frozen after the first append (RAW
     * path) or first decoded frame (MP3 path). Subsequent appends
     * that disagree are rejected. 0 = not yet configured.
     */
    int32 ConfiguredSampleRate  = 0;
    int32 ConfiguredNumChannels = 0;

    /** Helper: validates / freezes the stream's format. Returns true
     *  if the incoming format is compatible. Sets up wave fields
     *  (SampleRate, NumChannels) on first use. */
    bool ResolveStreamFormat(int32 InSampleRate, int32 InNumChannels);

    /** Broadcast an error to OnAudioError on the game thread. Safe
     *  to call from any thread. */
    void BroadcastAudioError(const FString& Message);
};
