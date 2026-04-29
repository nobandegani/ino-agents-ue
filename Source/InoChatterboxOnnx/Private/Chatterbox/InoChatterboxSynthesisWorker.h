// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "Templates/Atomic.h"
#include "Templates/UniquePtr.h"

#include "Chatterbox/InoChatterboxTypes.h"

class FInoChatterboxModels;
class FInoChatterboxTokenizer;
class FRunnableThread;
class FEvent;

/**
 * FRunnable that runs Chatterbox Turbo synthesis jobs on a dedicated
 * worker thread, FIFO order.
 *
 * This is NOT a UObject. It is a plain C++ class held in a
 * TUniquePtr<FInoChatterboxSynthesisWorker> by UInoChatterboxTtsSubsystem.
 * Non-copyable, non-movable: it owns a thread, an FEvent, and borrows
 * references to a model bundle / tokenizer that must outlive it.
 *
 * Lifecycle contract (enforced by the subsystem):
 *
 *   1. Constructed on the game thread AFTER Models + Tokenizer are
 *      successfully loaded. Stores const-refs to both. The worker
 *      borrows them; it never owns them.
 *   2. Spawns its FRunnableThread from its own constructor — the
 *      worker thread begins running Run() as soon as construction
 *      returns.
 *   3. Game thread produces work via Enqueue(FPendingSynth). The SPSC
 *      queue is safe for that pattern; the internal FEvent wakes the
 *      worker if it was idle.
 *   4. Game thread can CancelAndFlush() at any time: flips
 *      bCancelCurrent (the Runner's AR loop samples this per iteration
 *      and exits within ~tens of ms), then drains all queued items
 *      and fires their OnComplete delegates synchronously on the game
 *      thread with a "cancelled" error.
 *   5. Destructor MUST run on the game thread, BEFORE Models /
 *      Tokenizer are reset on the subsystem. The destructor sets stop
 *      + cancel, triggers the event, joins the thread, returns the
 *      FEvent to the pool, and fires "worker shutting down" errors
 *      for any items that were still queued.
 *
 * Threading model:
 *   - Game thread writes: Enqueue, CancelAndFlush, Stop (from FRunnable),
 *     destructor.
 *   - Worker thread reads: the queue head + the three atomic flags.
 *   - The internal FEvent (auto-reset) synchronises idle-wait/wake.
 *   - OnComplete delegates are always invoked on the game thread via
 *     AsyncTask(ENamedThreads::GameThread, ...), never directly from
 *     the worker — Blueprint-bound UFUNCTIONs are not thread-safe.
 */
class FInoChatterboxSynthesisWorker : public FRunnable
{
public:
    /**
     * One pending synthesis request. Self-contained so the worker
     * doesn't have to reach back into the subsystem for anything.
     */
    struct FPendingSynth
    {
        /** User text (may contain paralinguistic tags like [laugh]). */
        FString Text;

        /** Reference audio at 24 kHz mono float32 in [-1, +1]. Already
         *  validated by the subsystem before enqueue — the worker just
         *  feeds it into the runner. */
        TArray<float> ReferenceAudio;

        /** MaxNewTokens + RepetitionPenalty, mapped to the runner's
         *  FSynthesisOptions verbatim. */
        FInoChatterboxSynthesisOptions Options;

        /** Fired on the game thread when this item finishes, whether
         *  success, cancel, or error. Always fires exactly once per
         *  enqueued item, even on shutdown. */
        FOnInoChatterboxSynthesisComplete OnComplete;

        /** Streaming cadence: > 0 means run the decoder every N
         *  generated tokens and dispatch OnAudioChunk for each incremental
         *  piece; 0 disables streaming (decoder runs once at the end).
         *  Ignored unless OnAudioChunk is bound. Forwarded verbatim to
         *  FInoChatterboxRunner::SynthesizeText. */
        int32 StreamChunkTokens = 0;

        /** Optional per-chunk callback. When bound, the worker dispatches
         *  this on the game thread (via AsyncTask) for every chunk the
         *  runner produces — int16 PCM LE bytes, matching the final
         *  FInoChatterboxSynthesisResult::AudioSamples format so consumers
         *  can append each chunk directly into a streaming audio buffer.
         *
         *  Semantics: the final chunk (bIsFinal=true) fires after every
         *  intermediate chunk AND before OnComplete — same ordering the
         *  runner enforces. Safe to leave unbound; the worker takes the
         *  non-streaming fast path in that case. */
        FOnInoChatterboxAudioChunk OnAudioChunk;
    };

    /**
     * Construct + spawn the worker thread. References must outlive the
     * worker (enforced by the subsystem: unload tears down the worker
     * before resetting Models/Tokenizer).
     */
    FInoChatterboxSynthesisWorker(
        const FInoChatterboxModels&    InModels,
        const FInoChatterboxTokenizer& InTokenizer);

    /**
     * Tears the worker down cleanly: flips stop + cancel, triggers the
     * queue event, joins the thread, drains the queue firing
     * "shutting down" errors for any remaining items. Safe to call
     * even if the thread never got a chance to start.
     *
     * MUST be called on the game thread. Blocks until the worker
     * thread joins — bounded to the time for the current AR iteration
     * to see the cancel flag and exit (typically tens of ms).
     */
    virtual ~FInoChatterboxSynthesisWorker();

    FInoChatterboxSynthesisWorker(const FInoChatterboxSynthesisWorker&) = delete;
    FInoChatterboxSynthesisWorker& operator=(const FInoChatterboxSynthesisWorker&) = delete;
    FInoChatterboxSynthesisWorker(FInoChatterboxSynthesisWorker&&) = delete;
    FInoChatterboxSynthesisWorker& operator=(FInoChatterboxSynthesisWorker&&) = delete;

    /**
     * Queue a synth request. Returns immediately; the worker picks it
     * up and fires OnComplete on the game thread when done. FIFO
     * order — multiple Enqueue calls in the same frame run in the
     * order they were made.
     *
     * MUST be called on the game thread (SPSC queue; the game thread
     * is the single producer).
     */
    void Enqueue(FPendingSynth Item);

    /**
     * Cooperatively cancel any in-flight synthesis + drop every queued
     * item, firing each item's OnComplete with bSuccess=false and
     * ErrorMessage="Cancelled". The in-flight synth's OnComplete fires
     * once the AR loop observes the cancel flag (tens of ms) and
     * early-returns from SynthesizeText.
     *
     * MUST be called on the game thread.
     *
     * Safe to call with nothing pending (no-op beyond clearing the
     * flags).
     */
    void CancelAndFlush();

    // --- FRunnable interface ---
    virtual uint32 Run() override;
    virtual void   Stop() override;

private:
    // Process one item on the worker thread. Builds an
    // FInoChatterboxRunner on the fly, runs SynthesizeText with the
    // cancel flag, packs the result into an FInoChatterboxSynthesisResult,
    // and dispatches OnComplete via AsyncTask to the game thread.
    void ProcessSynth(FPendingSynth& Item);

    // Borrowed — the subsystem guarantees these outlive the worker.
    const FInoChatterboxModels&    Models;
    const FInoChatterboxTokenizer& Tokenizer;

    // SPSC queue: the game thread is the single producer via Enqueue;
    // the worker thread is the single consumer via Run.
    TQueue<FPendingSynth, EQueueMode::Spsc> Queue;

    // Auto-reset event. Triggered by Enqueue (new work), Stop (shutdown),
    // and CancelAndFlush (interrupt idle wait). Returned to the pool
    // in the destructor after the worker thread joins.
    FEvent* QueueEvent = nullptr;

    // Set once at shutdown: tells Run() to drain the queue and return.
    TAtomic<bool> bStopRequested{false};

    // Flips the Runner's cancel flag so the current AR loop exits.
    // Reset at the start of each ProcessSynth. Set by CancelAndFlush
    // (explicit cancel) and by the destructor (shutdown).
    TAtomic<bool> bCancelCurrent{false};

    // Worker thread handle. Constructed by this class's constructor,
    // destroyed (joined) in the destructor.
    TUniquePtr<FRunnableThread> Thread;
};
