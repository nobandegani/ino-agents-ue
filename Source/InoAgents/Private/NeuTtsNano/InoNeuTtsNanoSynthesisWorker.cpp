// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsNanoSynthesisWorker.h"

#include "InoAgentsLog.h"
#include "InoNeuTtsNanoRunner.h"
#include "InoNeuTtsNanoVoiceRegistry.h"
#include "NeuTtsNano/InoNeuTtsNanoSubsystem.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"

FInoNeuTtsNanoSynthesisWorker::FInoNeuTtsNanoSynthesisWorker(
    TWeakObjectPtr<UInoNeuTtsNanoSubsystem> InOwner,
    FInoNeuTtsNanoRunner* InRunner,
    const FInoNeuTtsNanoVoiceRegistry* InVoiceRegistry)
    : WeakSubsystem(InOwner)
    , Runner(InRunner)
    , VoiceRegistry(InVoiceRegistry)
{
    // Auto-reset event: Trigger() on each Enqueue + on Stop(), Wait()
    // in Run() to block when the queue is empty. Milestone 4 flips the
    // Trigger producer from just Stop() to Enqueue + Stop.
    QueueEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/ false);

    // Start the worker thread. Name shows up in profilers + crash
    // stacks — keep it searchable.
    Thread = FRunnableThread::Create(
        this,
        TEXT("InoNeuTtsNanoSynthesisWorker"),
        /*StackSize=*/ 0,
        TPri_Normal);
}

FInoNeuTtsNanoSynthesisWorker::~FInoNeuTtsNanoSynthesisWorker()
{
    // Signal shutdown + unblock the worker if it's waiting on the
    // queue event. Both flags are safe to set multiple times.
    bStopRequested = true;
    bStreamCancelled = true;

    if (QueueEvent != nullptr)
    {
        QueueEvent->Trigger();
    }

    // Join the thread. WaitForCompletion blocks until Run() returns.
    if (Thread != nullptr)
    {
        Thread->WaitForCompletion();
        delete Thread;
        Thread = nullptr;
    }

    // Return the event to the pool last — no-one else can access it
    // now that the thread has joined.
    if (QueueEvent != nullptr)
    {
        FPlatformProcess::ReturnSynchEventToPool(QueueEvent);
        QueueEvent = nullptr;
    }
}

uint32 FInoNeuTtsNanoSynthesisWorker::Run()
{
    UE_LOG(LogInoAgents, Verbose, TEXT("NeuTtsNano worker: thread started"));

    // Milestone 3 skeleton: idle loop. Wakes on Stop() trigger and
    // exits. Milestone 4 replaces the body with: dequeue a pending
    // synth, build the prompt, run the AR decode loop (cancel-checked),
    // parse speech tokens, run the codec decoder, convert to int16
    // PCM, AsyncTask back to game thread.
    while (!bStopRequested.Load())
    {
        if (QueueEvent != nullptr)
        {
            QueueEvent->Wait();
        }
    }

    UE_LOG(LogInoAgents, Verbose, TEXT("NeuTtsNano worker: thread exiting"));
    return 0;
}

void FInoNeuTtsNanoSynthesisWorker::Stop()
{
    // Called by UE's FRunnableThread shutdown path (the dtor already
    // handles bStopRequested + event trigger, so this is just an extra
    // entry point for engine-driven teardown).
    bStopRequested = true;
    bStreamCancelled = true;
    if (QueueEvent != nullptr)
    {
        QueueEvent->Trigger();
    }
}

void FInoNeuTtsNanoSynthesisWorker::SignalCancel()
{
    bStreamCancelled = true;
    // Don't trigger QueueEvent — cancellation means "abandon current
    // work when the AR loop next checks", not "wake from idle". In
    // Milestone 4 the AR loop reads bStreamCancelled directly.
}
