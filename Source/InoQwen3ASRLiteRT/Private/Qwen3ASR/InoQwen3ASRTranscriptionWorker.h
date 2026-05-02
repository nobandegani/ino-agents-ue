// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Containers/Queue.h"
#include "UObject/WeakObjectPtr.h"
#include "Qwen3ASR/InoQwen3ASRLiteRTTypes.h"

#include <atomic>

class FInoQwen3ASRRunner;

/**
 * One-thread serial transcription worker. Owns no state beyond a pointer to
 * an externally-owned FInoQwen3ASRRunner; the subsystem creates this worker
 * after a successful model load and destroys it on Deinitialize.
 *
 * Multiple TranscribeAsync calls from the game thread enqueue here. The
 * worker pops one request at a time, runs the full mel→encode→decode→
 * detokenize pipeline, and dispatches the completion delegate back to the
 * game thread via AsyncTask. All shared state crosses thread boundaries
 * via FEvent + Spsc queue + atomic — no game-thread locks held during
 * inference.
 *
 * Cancellation: setting bCancelCurrent.Load() true causes the runner's
 * decode loop to abort early. Currently the cancel check happens between
 * decoder steps inside the runner (Phase 4 work — for now Cancel only
 * affects requests that haven't started yet by clearing the queue).
 */
class FInoQwen3ASRTranscriptionWorker : public FRunnable
{
public:
    /**
     * @param InRunner  Externally owned, must outlive this worker.
     */
    explicit FInoQwen3ASRTranscriptionWorker(FInoQwen3ASRRunner* InRunner);
    virtual ~FInoQwen3ASRTranscriptionWorker();

    /**
     * Enqueue a transcription request. The audio is moved (zero-copy after
     * the move). When inference completes, OnComplete will be invoked on
     * the game thread; if Owner becomes invalid before that, the dispatch
     * is silently skipped.
     */
    void EnqueueRequest(
        TArray<float>&& AudioSamples,
        TWeakObjectPtr<UObject> Owner,
        FOnInoQwen3ASRTranscribeComplete OnComplete);

    /**
     * Drop every queued-but-not-yet-started request (each fires its
     * OnComplete with bSuccess=false, ErrorMessage="Cancelled" on the
     * game thread). The currently-running request, if any, runs to
     * completion — fine-grained mid-decode cancel is Phase 4 work.
     */
    void CancelPending();

    /** Approximate count of pending requests (one is being processed; the rest are queued). */
    int32 GetQueueDepthApprox() const;

    //~ FRunnable
    virtual uint32 Run() override;
    virtual void Stop() override;
    //~ End of FRunnable

private:
    struct FRequest
    {
        TArray<float> AudioSamples;
        TWeakObjectPtr<UObject> Owner;
        FOnInoQwen3ASRTranscribeComplete OnComplete;
    };

    /** Pointer to externally-owned runner. */
    FInoQwen3ASRRunner* Runner = nullptr;

    /** Spsc safe — one producer (game thread), one consumer (worker thread). */
    TQueue<TUniquePtr<FRequest>, EQueueMode::Spsc> Queue;

    /** Running counter for fast queue-depth queries (atomic). */
    std::atomic<int32> QueueDepth { 0 };

    /** Wakes the worker when a request is enqueued or Stop is called. */
    FEvent* WakeEvent = nullptr;

    /** Set true by Stop; the worker exits its loop when next woken. */
    std::atomic<bool> bShouldExit { false };

    /** Thread handle. Created in ctor, joined in dtor. */
    FRunnableThread* Thread = nullptr;

    /** Drain the queue, dispatching cancellation results to the game thread. */
    void DrainQueueAsCancelled();

    /** Run one request through the pipeline + dispatch result to game thread. */
    void ProcessRequest(FRequest& Req);
};
