// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxSynthesisWorker.h"

#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"

#include "Audio/InoAudioFunctionLibrary.h"
#include "InoAgentsLog.h"
#include "InoChatterboxModels.h"
#include "InoChatterboxRunner.h"
#include "InoChatterboxTokenizer.h"

namespace
{
    /**
     * Dispatch a failure delegate for a single FPendingSynth on the
     * game thread. Used during flush + shutdown to clear queued items
     * without running synthesis on them.
     *
     * We copy the delegate + message by value into the AsyncTask
     * lambda — the FPendingSynth itself may already be gone by the
     * time the lambda runs.
     */
    void DispatchFailureOnGameThread(
        const FOnInoChatterboxSynthesisComplete& OnComplete,
        const FString& ErrMessage)
    {
        AsyncTask(ENamedThreads::GameThread,
            [OnComplete, ErrMessage]()
        {
            FInoChatterboxSynthesisResult Empty;
            OnComplete.ExecuteIfBound(false, Empty, ErrMessage);
        });
    }
}

FInoChatterboxSynthesisWorker::FInoChatterboxSynthesisWorker(
    const FInoChatterboxModels&    InModels,
    const FInoChatterboxTokenizer& InTokenizer)
    : Models(InModels)
    , Tokenizer(InTokenizer)
{
    // Auto-reset event: every Trigger wakes at most one Wait. Matches
    // the single-consumer pattern of our Run() loop.
    QueueEvent = FGenericPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/ false);

    // Name is visible in profilers + crash dumps. BPri_Normal is fine —
    // synthesis is not latency-critical relative to game-frame deadlines.
    Thread.Reset(FRunnableThread::Create(
        this,
        TEXT("InoChatterboxSynthesisWorker"),
        /*InStackSize=*/ 0,
        TPri_Normal));

    if (!Thread.IsValid())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("Chatterbox: Worker: FRunnableThread::Create returned null ")
               TEXT("-- subsequent synthesis will fail immediately"));
    }
    else
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox: Worker: thread started"));
    }
}

FInoChatterboxSynthesisWorker::~FInoChatterboxSynthesisWorker()
{
    check(IsInGameThread());

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Worker: shutting down (destructor)"));

    // 1. Tell the worker to bail ASAP.
    bStopRequested.Store(true);
    bCancelCurrent.Store(true);

    // 2. Wake the thread if it's idle on QueueEvent->Wait.
    if (QueueEvent)
    {
        QueueEvent->Trigger();
    }

    // 3. Join. Blocks until the thread's Run() returns (bounded to
    //    the current AR iteration's duration, typically tens of ms).
    if (Thread.IsValid())
    {
        Thread->WaitForCompletion();
        Thread.Reset();
    }

    // 4. The thread is gone. Drain anything the worker didn't get to
    //    and fire "shutting down" errors so Blueprint observers don't
    //    see dangling OnComplete delegates.
    int32 NumDrained = 0;
    FPendingSynth Item;
    while (Queue.Dequeue(Item))
    {
        DispatchFailureOnGameThread(
            Item.OnComplete,
            TEXT("Chatterbox synthesis cancelled (worker shutting down)"));
        ++NumDrained;
    }
    if (NumDrained > 0)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("Chatterbox: Worker: Stop observed -- draining queue (items=%d)"),
               NumDrained);
    }

    // 5. Return the event to the pool.
    if (QueueEvent)
    {
        FGenericPlatformProcess::ReturnSynchEventToPool(QueueEvent);
        QueueEvent = nullptr;
    }
}

void FInoChatterboxSynthesisWorker::Enqueue(FPendingSynth Item)
{
    check(IsInGameThread());

    if (bStopRequested.Load())
    {
        // Worker is shutting down; do not enqueue, fire failure
        // synchronously. This path is rare — it happens if a caller
        // races Enqueue against UnloadModels on the same frame.
        UE_LOG(LogInoAgents, Warning,
               TEXT("Chatterbox: Worker: Enqueue rejected -- worker is shutting down"));
        DispatchFailureOnGameThread(
            Item.OnComplete,
            TEXT("Chatterbox synthesis rejected (worker is shutting down)"));
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Worker: Enqueue (text_len=%d, ref_audio_bytes=%d, ")
           TEXT("max_new_tokens=%d, stream_chunk_tokens=%d)"),
           Item.Text.Len(), Item.ReferenceAudio.Num() * (int32)sizeof(float),
           Item.Options.MaxNewTokens, Item.StreamChunkTokens);

    Queue.Enqueue(MoveTemp(Item));

    if (QueueEvent)
    {
        QueueEvent->Trigger();
    }
}

void FInoChatterboxSynthesisWorker::CancelAndFlush()
{
    check(IsInGameThread());

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Worker: CancelAndFlush"));

    // Flip cancel so any in-flight SynthesizeText exits at its next
    // AR iteration. The worker will clear bCancelCurrent at the top
    // of the next ProcessSynth if any items follow.
    bCancelCurrent.Store(true);

    // Drain queued items and fire "cancelled" for each. Done on the
    // game thread (we're on the game thread right now), so these
    // delegates fire in the caller's current frame — matches Blueprint
    // expectations for "I clicked cancel; the queue is empty now".
    int32 NumDrained = 0;
    FPendingSynth Item;
    while (Queue.Dequeue(Item))
    {
        // Synchronous fire — no AsyncTask needed, we're already on GT.
        FInoChatterboxSynthesisResult Empty;
        Item.OnComplete.ExecuteIfBound(
            false, Empty, TEXT("Chatterbox synthesis cancelled"));
        ++NumDrained;
    }
    if (NumDrained > 0)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox: Worker: CancelAndFlush -- drained %d queued item(s)"),
               NumDrained);
    }

    // Wake the worker in case it was idle — this lets it see
    // bCancelCurrent quickly (though the runner's check is what
    // actually causes the in-flight SynthesizeText to return).
    if (QueueEvent)
    {
        QueueEvent->Trigger();
    }
}

uint32 FInoChatterboxSynthesisWorker::Run()
{
    // Worker thread main loop. Waits for work on QueueEvent, drains
    // the queue FIFO, processes each item to completion (potentially
    // seconds), loops back to wait. Exits when bStopRequested is
    // observed true.

    int32 NumProcessed = 0;
    while (!bStopRequested.Load())
    {
        // Idle wait. Auto-reset event → Trigger wakes exactly one Wait;
        // subsequent Triggers are lost (which is fine — if there are
        // multiple items queued we loop through them all before waiting
        // again).
        if (QueueEvent)
        {
            QueueEvent->Wait();
        }

        // Drain everything available. Check stop between items so a
        // shutdown during a long queue doesn't have to wait for the
        // whole queue.
        while (!bStopRequested.Load())
        {
            FPendingSynth Item;
            if (!Queue.Dequeue(Item))
            {
                break;   // queue empty, go back to wait
            }
            UE_LOG(LogInoAgents, Log,
                   TEXT("Chatterbox: Worker: dequeued synth item (text_len=%d)"),
                   Item.Text.Len());
            ProcessSynth(Item);
            ++NumProcessed;
        }
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Worker: thread exiting (processed=%d)"),
           NumProcessed);

    // On shutdown, the destructor drains any leftover items and fires
    // their failure delegates — we don't need to do it here.
    return 0;
}

void FInoChatterboxSynthesisWorker::Stop()
{
    // FRunnable::Stop is called from the game thread by UE's runnable
    // machinery on thread teardown — we also call it from the
    // destructor path manually. Safe to call multiple times.
    bStopRequested.Store(true);
    bCancelCurrent.Store(true);

    if (QueueEvent)
    {
        QueueEvent->Trigger();
    }
}

void FInoChatterboxSynthesisWorker::ProcessSynth(FPendingSynth& Item)
{
    // Start with a clean cancel flag — a prior CancelAndFlush would
    // have set it true, but the items following the flushed ones are
    // supposed to run normally. (If the caller wants them also
    // cancelled they'd have dropped them before they got queued.)
    bCancelCurrent.Store(false);

    const double TStart = FPlatformTime::Seconds();

    FInoChatterboxRunner Runner(Models, Tokenizer);

    FInoChatterboxRunner::FSynthesisOptions RunnerOpts;
    RunnerOpts.MaxNewTokens      = Item.Options.MaxNewTokens;
    RunnerOpts.RepetitionPenalty = Item.Options.RepetitionPenalty;

    // Wire the streaming callback only when the caller bound one —
    // otherwise pass an empty TFunction and the runner's intermediate-
    // and final-chunk dispatch becomes a no-op (runner checks
    // `if (!OnChunk)` before calling). Both SynthesizeAsync (no
    // streaming) and SynthesizeStreamAsync use this same ProcessSynth;
    // the difference is purely "is OnAudioChunk bound".
    const bool bStreaming =
        Item.OnAudioChunk.IsBound() && Item.StreamChunkTokens > 0;

    // Delegate is captured by value (copy) into the runner's TFunction
    // so the AsyncTask closure below can still use it even after
    // ProcessSynth returns (the TFunction outlives the stack frame
    // only until SynthesizeText returns, but each invocation runs
    // synchronously on this thread and AsyncTask captures a fresh
    // copy into its own closure before returning). No lifetime risk.
    const FOnInoChatterboxAudioChunk OnAudioChunkCopy = Item.OnAudioChunk;

    FInoChatterboxRunner::FOnStreamChunk StreamCb;
    if (bStreaming)
    {
        StreamCb = [OnAudioChunkCopy](
            TArrayView<const float> NewSamples,
            int32                   NumGenTokens,
            bool                    bIsFinal)
        {
            // Quantize float32 → int16 PCM LE bytes on the worker
            // thread (cheap: 2× multiply-clamp per sample, sub-ms for
            // a few thousand samples). The AsyncTask below ships the
            // bytes by move — no extra copy when the lambda body runs
            // on the game thread.
            TArray<uint8> PcmBytes;
            UInoAudioFunctionLibrary::Float32ToInt16PcmBytesMono(NewSamples, PcmBytes);

            AsyncTask(ENamedThreads::GameThread,
                [OnAudioChunkCopy,
                 PcmBytes = MoveTemp(PcmBytes),
                 NumGenTokens, bIsFinal]()
            {
                OnAudioChunkCopy.ExecuteIfBound(PcmBytes, bIsFinal, NumGenTokens);
            });
        };
    }

    FInoChatterboxRunner::FSynthesisResult NativeResult;
    FString NativeError;
    const bool bOK = Runner.SynthesizeText(
        Item.Text,
        MakeArrayView(Item.ReferenceAudio),
        RunnerOpts,
        NativeResult,
        &NativeError,
        &bCancelCurrent,
        bStreaming ? Item.StreamChunkTokens : 0,
        StreamCb);

    if (!bOK)
    {
        if (bCancelCurrent.Load(EMemoryOrder::Relaxed))
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("Chatterbox: Worker: Cancel observed during AR loop (after %.1f ms)"),
                   (FPlatformTime::Seconds() - TStart) * 1000.0);
        }
        else
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("Chatterbox: Worker: synth FAILED in %.1f ms: %s"),
                   (FPlatformTime::Seconds() - TStart) * 1000.0, *NativeError);
        }
    }
    else
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox: Worker: synth complete -- %d samples, %d tokens, %.1f ms"),
               NativeResult.AudioSamples.Num(),
               NativeResult.NumGeneratedTokens,
               (FPlatformTime::Seconds() - TStart) * 1000.0);
    }

    // Build the Blueprint-visible result regardless of success — timings
    // are still useful on failure (e.g. "we got 500 ms in before cancel").
    //
    // Quantize the runner's float32 samples into int16 PCM LE bytes
    // for the Blueprint surface. Matches FInoChatterboxSynthesisResult::
    // AudioSamples's documented format. A 1-2 s utterance quantizes in
    // well under a millisecond on a worker thread.
    FInoChatterboxSynthesisResult BpResult;
    UInoAudioFunctionLibrary::Float32ToInt16PcmBytesMono(
        MakeArrayView(NativeResult.AudioSamples), BpResult.AudioSamples);
    // SampleRate is not a field on BpResult — consumers pull it from
    // UInoChatterboxTtsSubsystem::GetOutputSampleRate() instead (single
    // source of truth, always 24000 for Turbo). DurationSeconds still
    // lives on the struct as a BP convenience; it's computed from the
    // runner's internal NativeResult.SampleRate, which is 24 kHz fixed
    // in the runner's Chatterbox Turbo constants.
    BpResult.DurationSeconds    =
        (NativeResult.SampleRate > 0)
            ? (float)NativeResult.AudioSamples.Num() / (float)NativeResult.SampleRate
            : 0.0f;
    BpResult.NumGeneratedTokens = NativeResult.NumGeneratedTokens;
    BpResult.bHitStopToken      = NativeResult.bHitStopToken;
    BpResult.TotalElapsedMs     = (float)((FPlatformTime::Seconds() - TStart) * 1000.0);
    BpResult.EncoderMs          = (float)NativeResult.EncoderMs;
    BpResult.EmbedTotalMs       = (float)NativeResult.EmbedTotalMs;
    BpResult.LanguageModelMs    = (float)NativeResult.LanguageModelMs;
    BpResult.DecoderMs          = (float)NativeResult.DecoderMs;

    // Hop back to the game thread to fire OnComplete. Capture by value
    // — OnComplete is a copy, BpResult moves into the lambda.
    const FOnInoChatterboxSynthesisComplete OnCompleteCopy = Item.OnComplete;
    AsyncTask(ENamedThreads::GameThread,
        [OnCompleteCopy, bOK, BpResult = MoveTemp(BpResult),
         NativeError = MoveTemp(NativeError)]() mutable
    {
        OnCompleteCopy.ExecuteIfBound(bOK, BpResult, NativeError);
    });
}
