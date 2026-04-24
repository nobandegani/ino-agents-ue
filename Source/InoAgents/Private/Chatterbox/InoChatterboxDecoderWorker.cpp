// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxDecoderWorker.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"

#include "InoAgentsLog.h"

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

FInoChatterboxDecoderWorker::FInoChatterboxDecoderWorker(FDecodeTask InTask)
    : Task(MoveTemp(InTask))
{
    // Auto-reset: AR thread Triggers, worker Waits exactly once per
    // Trigger. If multiple Triggers arrive while the worker is mid-
    // Task, only one is latched — fine, the worker drains
    // bRequestPending internally before Wait-ing again.
    WakeEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/ false);

    // Manual-reset: stays signaled until explicitly Reset. Used by
    // WaitForIdle — once the worker Triggers this, any number of AR
    // WaitForIdle() calls return without blocking, until the next
    // Publish() Reset's it. Starts signaled (we're idle on birth).
    IdleEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/ true);
    if (IdleEvent)
    {
        IdleEvent->Trigger();
    }

    Thread.Reset(FRunnableThread::Create(
        this,
        TEXT("InoChatterboxDecoderWorker"),
        /*InStackSize=*/ 0,
        TPri_Normal));

    if (!Thread.IsValid())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("Chatterbox: Decoder: FRunnableThread::Create returned null -- ")
               TEXT("streaming decoder path will not make progress"));
    }
    else
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox: Decoder: thread started"));
    }
}

FInoChatterboxDecoderWorker::~FInoChatterboxDecoderWorker()
{
    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Decoder: shutting down (destructor)"));

    // 1. Signal stop.
    {
        FScopeLock Lock(&StateLock);
        bStopRequested = true;
    }

    // 2. Wake the worker if it's blocked on WakeEvent->Wait.
    if (WakeEvent)
    {
        WakeEvent->Trigger();
    }

    // 3. Join. Bounded by one decoder Run() — ORT Run is atomic from
    //    our side; we can't abort it mid-flight. So the worker observes
    //    bStopRequested at the top of its next inner-loop iteration and
    //    returns 0.
    if (Thread.IsValid())
    {
        Thread->WaitForCompletion();
        Thread.Reset();
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Decoder: thread exited"));

    // 4. Return events to the pool.
    if (WakeEvent)
    {
        FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
        WakeEvent = nullptr;
    }
    if (IdleEvent)
    {
        FPlatformProcess::ReturnSynchEventToPool(IdleEvent);
        IdleEvent = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Producer-side API (AR thread)
// ---------------------------------------------------------------------------

void FInoChatterboxDecoderWorker::Publish(
    TArray<int64>&& Tokens, int32 NumGenTokens)
{
    const int32 TokenCount = Tokens.Num();
    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Decoder: request published (tokens=%d, num_gen_tokens=%d)"),
           TokenCount, NumGenTokens);

    // Defensive: if the worker thread failed to spawn in the
    // constructor, Publish is a no-op + record an error so the AR
    // thread sees it at the next HasError() check. Without this,
    // bRequestPending would be set with nobody ever clearing it,
    // and WaitForIdle() would block forever.
    if (!Thread.IsValid())
    {
        FScopeLock Lock(&StateLock);
        if (!bHasError)
        {
            bHasError = true;
            ErrorMsg  = TEXT("decoder worker thread failed to spawn");
        }
        return;
    }

    {
        FScopeLock Lock(&StateLock);
        // Single-slot overwrite — the previous pending-but-not-yet-
        // picked-up request is silently discarded. MoveTemp-ing into
        // PendingTokens frees the old buffer cleanly.
        PendingTokens       = MoveTemp(Tokens);
        PendingNumGenTokens = NumGenTokens;
        bRequestPending     = true;

        // We're no longer idle — reset the manual-reset event so any
        // subsequent WaitForIdle blocks. Reset is idempotent; harmless
        // if it was already unset.
        if (IdleEvent)
        {
            IdleEvent->Reset();
        }
    }

    // Wake the worker. Safe to call while holding nothing — the event
    // is a standalone kernel primitive.
    if (WakeEvent)
    {
        WakeEvent->Trigger();
    }
}

void FInoChatterboxDecoderWorker::WaitForIdle()
{
    UE_LOG(LogInoAgents, Verbose,
           TEXT("Chatterbox: Decoder: WaitForIdle begin"));
    const double WaitT0 = FPlatformTime::Seconds();

    // Fast path: already idle. Checking under the lock avoids a stale
    // read where Publish() has set bRequestPending but hasn't Reset
    // IdleEvent yet (not possible with the current impl — both are
    // done under StateLock — but the lock also makes the check atomic
    // against a racing Publish from another AR frame, should that
    // ever be the case).
    {
        FScopeLock Lock(&StateLock);
        if (!bRequestPending && !bDecoderWorking)
        {
            UE_LOG(LogInoAgents, Verbose,
                   TEXT("Chatterbox: Decoder: WaitForIdle complete (waited=0.0 ms, fast_path)"));
            return;
        }
    }

    // Slow path: block on the manual-reset event. The worker Triggers
    // it after completing Task when no more requests are pending.
    // Because the event is manual-reset, spurious early wakes don't
    // happen — once Triggered it stays signaled and Wait returns
    // immediately. The next Publish() will Reset it under the lock.
    if (IdleEvent)
    {
        IdleEvent->Wait();
    }
    const double WaitedMs = (FPlatformTime::Seconds() - WaitT0) * 1000.0;
    UE_LOG(LogInoAgents, Verbose,
           TEXT("Chatterbox: Decoder: WaitForIdle complete (waited=%.1f ms)"),
           WaitedMs);
}

bool FInoChatterboxDecoderWorker::HasError(FString& OutError)
{
    FScopeLock Lock(&StateLock);
    if (bHasError)
    {
        OutError = ErrorMsg;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Consumer-side loop (worker thread)
// ---------------------------------------------------------------------------

uint32 FInoChatterboxDecoderWorker::Run()
{
    while (true)
    {
        // Outer wait: block when there's nothing to do.
        if (WakeEvent)
        {
            WakeEvent->Wait();
        }

        // Inner drain loop: handle all pending work that may have
        // accumulated while we were processing the previous item.
        // Since Publish is single-slot this is at most one extra
        // item per outer-Wait cycle, but the loop structure costs
        // nothing and makes the race analysis simpler.
        while (true)
        {
            TArray<int64> LocalTokens;
            int32         LocalNumGen  = 0;
            bool          bShouldExit  = false;
            bool          bHaveWork    = false;

            {
                FScopeLock Lock(&StateLock);
                if (bStopRequested)
                {
                    bShouldExit = true;
                }
                else if (bRequestPending)
                {
                    LocalTokens         = MoveTemp(PendingTokens);
                    LocalNumGen         = PendingNumGenTokens;
                    bRequestPending     = false;
                    bDecoderWorking     = true;
                    bHaveWork           = true;
                }
                // else: no pending work AND no stop. Fall through to
                // break inner loop and re-Wait on WakeEvent.
            }

            if (bShouldExit)
            {
                return 0;
            }
            if (!bHaveWork)
            {
                break;  // go back to outer WakeEvent->Wait
            }

            // If a prior Task latched an error, don't run any more
            // decodes — the AR loop will observe HasError() and Fail
            // at its next WaitForIdle. Continuing to run ORT calls
            // that we know will fail wastes CPU and log spam. We
            // still consume the pending request (cleared above under
            // StateLock) so the bookkeeping stays consistent; just
            // skip the Task invocation.
            {
                FScopeLock Lock(&StateLock);
                if (bHasError)
                {
                    bDecoderWorking = false;
                    if (!bRequestPending && IdleEvent)
                    {
                        IdleEvent->Trigger();
                    }
                    continue;  // loop back to check for stop / next request
                }
            }

            // Run the Task without holding the lock. Task does the ORT
            // decoder Run + EmitDeltaChunk; it's the hot path of the
            // worker (tens to hundreds of ms). Holding StateLock here
            // would pointlessly block Publish() on the AR thread.
            UE_LOG(LogInoAgents, Log,
                   TEXT("Chatterbox: Decoder: Task begin (tokens=%d, num_gen_tokens=%d)"),
                   LocalTokens.Num(), LocalNumGen);
            const double TaskT0 = FPlatformTime::Seconds();
            FString LocalErr;
            const bool bOK = Task(LocalTokens, LocalNumGen, LocalErr);
            const double TaskMs = (FPlatformTime::Seconds() - TaskT0) * 1000.0;

            if (bOK)
            {
                UE_LOG(LogInoAgents, Log,
                       TEXT("Chatterbox: Decoder: Task complete in %.1f ms"),
                       TaskMs);
            }
            else
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Chatterbox: Decoder: Task FAILED in %.1f ms: %s"),
                       TaskMs, *LocalErr);
            }

            {
                FScopeLock Lock(&StateLock);
                bDecoderWorking = false;
                if (!bOK && !bHasError)
                {
                    // Latch the first error; subsequent Task calls'
                    // errors are dropped (we don't have a queue of
                    // error messages and the first one is usually the
                    // most informative anyway).
                    bHasError = true;
                    ErrorMsg  = LocalErr;
                }
                // If another Publish arrived while we were running
                // Task, the inner loop will pick it up on the next
                // iteration. Only Trigger IdleEvent when we're truly
                // idle — no pending work AND not working. This keeps
                // WaitForIdle() accurate.
                if (!bRequestPending)
                {
                    if (IdleEvent)
                    {
                        IdleEvent->Trigger();
                    }
                }
            }
        }
    }
    return 0;
}

void FInoChatterboxDecoderWorker::Stop()
{
    // FRunnable::Stop is invoked by UE's runnable machinery on thread
    // teardown. Our destructor also calls this path manually for
    // determinism. Safe to call more than once.
    {
        FScopeLock Lock(&StateLock);
        bStopRequested = true;
    }
    if (WakeEvent)
    {
        WakeEvent->Trigger();
    }
}
