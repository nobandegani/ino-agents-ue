// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"

class FRunnableThread;
class FEvent;

/**
 * FInoChatterboxTurboNativeDecoderWorker — a single-thread "latest request wins"
 * queue that runs the Chatterbox conditional_decoder off the AR-loop
 * thread.
 *
 * Motivation: the AR loop (language_model forward passes) and the
 * decoder are INDEPENDENT ORT sessions, so they can in principle run
 * concurrently. Our streaming pipeline naturally wants this — every
 * StreamChunkTokens AR iterations we need to decode a growing prefix,
 * and if we run the decoder inline the AR loop blocks for the decode's
 * duration. For a 200-token utterance the total decoder work during
 * streaming can approach the LM work in wallclock — so putting them
 * on different threads roughly halves the total streaming synth time.
 *
 * Design: single-slot queue. Publish() overwrites any pending-but-
 * not-yet-picked-up request with the new tokens. This models "always
 * decode the freshest prefix"; if the LM is fast and fires a second
 * chunk boundary before the decoder has picked up the first, the
 * first is discarded (never decoded, never emitted) and the decoder
 * goes straight to the newer one. Consumers get fewer chunks than
 * StreamChunkTokens would suggest in pathological cases, but always
 * the most-current prefix. In the common case decoder-per-chunk time
 * < LM-per-chunk time so no requests ever get superseded.
 *
 * Ownership / lifecycle:
 *   - Constructed by FInoChatterboxTurboNativeRunner::SynthesizeText when
 *     streaming is enabled. Lives only for the duration of ONE
 *     synthesis call.
 *   - Caller-supplied Task functor runs on the worker thread and is
 *     called with the captured prefix tokens + cumulative generated
 *     token count. Task is the runner's `RunDecoder + EmitDeltaChunk`
 *     chain, closed over the runner's local state. Because Task's
 *     captures are references into runner-local variables that are
 *     only safe for the worker thread to touch while the AR thread
 *     is NOT also running them — callers guarantee this via the
 *     WaitForIdle() contract (see below).
 *   - Destructor (~FInoChatterboxTurboNativeDecoderWorker) signals stop,
 *     joins the thread, returns both FEvents to the pool. Safe to
 *     call from the AR thread.
 *
 * Contracts the caller must honor:
 *   - Publish() is game-thread-AR-side only (single producer).
 *   - Before the AR thread itself calls any code that touches the
 *     same state Task touches (e.g. the final decode, which shares
 *     StreamAudioBuffer / DecInputsStore with the intermediate
 *     decodes), it MUST call WaitForIdle() to guarantee the worker
 *     is not mid-Task. Otherwise two threads race on StreamAudioBuffer
 *     and DecInputsStore[0].
 *   - HasError() samples a flag the worker set after a Task failure;
 *     check it after WaitForIdle() and propagate. The worker keeps
 *     processing subsequent requests even after an error (so a late
 *     cancel still completes), but signals the error through the
 *     HasError() bit.
 *
 * Threading of Task's callback target:
 *   Task runs on the worker thread. If Task ends up dispatching the
 *   game-thread callback (OnAudioChunk) via AsyncTask, that works
 *   regardless of which thread invoked Task. Same pattern as the
 *   existing FInoChatterboxTurboNativeSynthesisWorker.
 */
class FInoChatterboxTurboNativeDecoderWorker : public FRunnable
{
public:
    /**
     * Functor signature: decode the given token span and emit a chunk.
     *
     *   Tokens        — full speech-token prefix to decode, already
     *                   concat(PromptTokens, generated[1:]).
     *   NumGenTokens  — tokens-so-far count to pass into EmitDeltaChunk
     *                   for the OnAudioChunk delegate.
     *   OutError      — on failure, filled with a human-readable msg.
     *   Return        — true on success.
     *
     * Task is invoked under the worker thread, never from the caller's
     * thread. It owns the decoding + emit; the worker just orchestrates
     * wake-up / queue management.
     */
    using FDecodeTask = TFunction<bool(
        const TArray<int64>& Tokens,
        int32                NumGenTokens,
        FString&             OutError)>;

    /**
     * Spawn the worker thread. Task is captured by move; it must stay
     * valid until this object's destructor runs.
     */
    explicit FInoChatterboxTurboNativeDecoderWorker(FDecodeTask InTask);

    /**
     * Signals stop, joins the thread, returns FEvents. Blocking;
     * bounded by one decoder Run() (typically tens to hundreds of ms).
     */
    virtual ~FInoChatterboxTurboNativeDecoderWorker();

    FInoChatterboxTurboNativeDecoderWorker(const FInoChatterboxTurboNativeDecoderWorker&) = delete;
    FInoChatterboxTurboNativeDecoderWorker& operator=(const FInoChatterboxTurboNativeDecoderWorker&) = delete;

    /**
     * Publish a new decode request. Overwrites any pending-but-not-yet-
     * picked-up request (the worker is single-slot — see class doc).
     *
     * Non-blocking. Safe to call from the AR thread.
     */
    void Publish(TArray<int64>&& Tokens, int32 NumGenTokens);

    /**
     * Block the calling thread until the worker is idle: no pending
     * request AND not currently running Task. Returns immediately if
     * already idle.
     *
     * Must be called by the AR thread before the AR thread itself runs
     * any code that mutates state the Task also mutates (the shared
     * decoder tensors / audio buffer / last-emit counter). Not doing so
     * is a data race.
     */
    void WaitForIdle();

    /**
     * Thread-safe check for a latched error from a previous Task call.
     * Returns true and fills OutError if any Task returned false; false
     * otherwise. Subsequent queries keep returning the same error until
     * the worker is destroyed — there is no "clear" operation.
     */
    bool HasError(FString& OutError);

    //~ FRunnable interface
    virtual uint32 Run() override;
    virtual void   Stop() override;

private:
    FDecodeTask Task;

    TUniquePtr<FRunnableThread> Thread;
    FEvent* WakeEvent = nullptr;   ///< auto-reset: AR thread signals, worker waits
    FEvent* IdleEvent = nullptr;   ///< manual-reset: worker signals idle, AR waits

    FCriticalSection StateLock;
    // All fields below are guarded by StateLock.
    bool          bRequestPending  = false;
    bool          bDecoderWorking  = false;
    bool          bStopRequested   = false;
    bool          bHasError        = false;
    TArray<int64> PendingTokens;
    int32         PendingNumGenTokens = 0;
    FString       ErrorMsg;
};
