// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Qwen3ASR/InoQwen3ASRLiteRTTypes.h"
#include "InoQwen3ASRLiteRTSubsystem.generated.h"

// Forward decl — full definitions live in Private/Qwen3ASR/. Keeping the
// runner / worker headers out of this public surface avoids dragging the
// LiteRT C API into every TU that consumes the subsystem.
class FInoQwen3ASRRunner;
class FInoQwen3ASRTranscriptionWorker;

/**
 * UInoQwen3ASRLiteRTSubsystem — Game-instance-scoped Qwen3-ASR runtime.
 *
 * Lifecycle:
 *   1. Subsystem auto-creates with the GameInstance.
 *   2. Caller invokes LoadModelAsync(Config, OnLoaded). The model + vocab
 *      load happens on a background thread (~600 ms). OnLoaded fires on
 *      the game thread when ready.
 *   3. Caller invokes TranscribeAudioAsync / TranscribeWavFileAsync any
 *      number of times. Each request is queued on the worker thread and
 *      processed serially (one in-flight at a time per subsystem); the
 *      OnComplete delegate fires on the game thread when each request
 *      finishes.
 *   4. CancelTranscription() drops all queued-but-not-started requests
 *      with a "Cancelled" error. The currently-running request, if any,
 *      runs to completion.
 *   5. Subsystem teardown stops the worker and unloads the model.
 *
 * Threading guarantees:
 *   - All public methods are safe to call from the game thread only.
 *   - All delegate invocations are dispatched onto the game thread.
 *   - No model state is touched from the game thread; the runner lives
 *     exclusively on the worker.
 *
 * Per-instance, single in-flight inference. If you need parallel
 * transcription (multiple audio clips at once), instantiate per-stream
 * subsystems — but be aware each holds the ~794 MB model in memory.
 */
UCLASS()
class INOQWEN3ASRLITERT_API UInoQwen3ASRLiteRTSubsystem
    : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    // Forward-declared private types in member TUniquePtrs require these to
    // be declared out-of-line so the deleter is instantiated in a TU that
    // has the full type. UHT auto-generates an FVTableHelper ctor for hot
    // reload; we declare it here too so its body lands in the same .cpp.
    UInoQwen3ASRLiteRTSubsystem();
    UInoQwen3ASRLiteRTSubsystem(FVTableHelper& Helper);
    virtual ~UInoQwen3ASRLiteRTSubsystem();

    //~ USubsystem
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    //~ End of USubsystem

    /**
     * Load model + tokenizer from disk asynchronously. OnLoaded fires
     * exactly once on the game thread when the load completes (success or
     * failure). Calling this while a load is already in flight returns
     * immediately and OnLoaded is invoked with bSuccess=false,
     * ErrorMessage="Already loading".
     */
    UFUNCTION(BlueprintCallable, Category = "Qwen3 ASR")
    void LoadModelAsync(
        const FInoQwen3ASRConfig& Config,
        const FOnInoQwen3ASRModelLoaded& OnLoaded);

    /**
     * Transcribe a buffer of 16 kHz mono float32 PCM samples. Up to the
     * first kAudioWindowSamples (= 80 000 = 5 s) are used; longer audio
     * is truncated for now (chunking is Phase 5 work). OnComplete fires
     * on the game thread.
     */
    UFUNCTION(BlueprintCallable, Category = "Qwen3 ASR")
    void TranscribeAudioAsync(
        const TArray<float>& AudioSamples,
        const FOnInoQwen3ASRTranscribeComplete& OnComplete);

    /**
     * Transcribe a WAV file from disk. The file must be 16 kHz mono PCM
     * (int16 or float32 little-endian) — pre-convert with ffmpeg if
     * yours isn't. OnComplete fires on the game thread.
     */
    UFUNCTION(BlueprintCallable, Category = "Qwen3 ASR")
    void TranscribeWavFileAsync(
        const FString& WavPath,
        const FOnInoQwen3ASRTranscribeComplete& OnComplete);

    /** True after a successful LoadModelAsync. */
    UFUNCTION(BlueprintPure, Category = "Qwen3 ASR")
    bool IsModelLoaded() const { return bModelLoaded; }

    /**
     * Drop all queued-but-not-started requests. The currently-running
     * request (if any) runs to completion — fine-grained mid-decode
     * cancel will land in Phase 5.
     */
    UFUNCTION(BlueprintCallable, Category = "Qwen3 ASR")
    void CancelTranscription();

    /** Approximate number of pending transcription requests in the queue. */
    UFUNCTION(BlueprintPure, Category = "Qwen3 ASR")
    int32 GetQueueDepth() const;

private:
    /** Resolve a relative filename to an absolute path inside the InoAgents plugin's Qwen3ASR/LiteRT/ tree. */
    static FString ResolveModelPath(const FString& FileName);
    static FString ResolveVocabPath(const FString& FileName);

    /** Set on the game thread once LoadModelAsync's background task succeeds. */
    bool bModelLoaded = false;

    /** Set on the game thread while an async LoadModelAsync is in flight. */
    bool bModelLoading = false;

    /** Owned. Created by LoadModelAsync's worker task on success, destroyed on Deinitialize. */
    TUniquePtr<FInoQwen3ASRRunner> Runner;

    /** Owned. Created when the runner is ready, destroyed before the runner is. */
    TUniquePtr<FInoQwen3ASRTranscriptionWorker> Worker;
};
