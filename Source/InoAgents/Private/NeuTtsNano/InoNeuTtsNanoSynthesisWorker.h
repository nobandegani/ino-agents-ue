// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"

#include "UObject/WeakObjectPtr.h"

#include "NeuTtsNano/InoNeuTtsNanoTypes.h"   // FInoNeuTtsNanoSynthesisOptions, FOn*

// Forward-decls — the worker's .cpp pulls in the real types.
class FInoNeuTtsNanoRunner;
class FInoNeuTtsNanoVoiceRegistry;
class UInoNeuTtsNanoSubsystem;
class FEvent;

/**
 * One queued synthesis request. Plain-struct, passed by value through
 * the Spsc queue. The dynamic delegate inside is a UObject-aware handle
 * — it's safe to copy into a TQueue entry and later invoke from the
 * game thread (via AsyncTask marshaling in the worker).
 */
struct FInoNeuTtsNanoPendingSynth
{
    FString PhonemesText;                              // pre-phonemized IPA
    FName   VoiceName;
    FInoNeuTtsNanoSynthesisOptions     Options;
    FOnInoNeuTtsNanoSynthesisComplete  OnComplete;     // dynamic — invoke on GT
};

/**
 * Dedicated synthesis worker thread for UInoNeuTtsNanoSubsystem.
 *
 * Single-thread-serializing consumer of:
 *   - llama_context (not thread-safe — only one decode at a time)
 *   - FInoOnnxSession (thread-safe for Run, but we serialise to
 *     keep cancellation semantics clean)
 *
 * Threading contract (Milestone 3 scope — expanded in Milestone 4):
 *   - One pinned FRunnableThread per worker instance.
 *   - Created + Stopped by UInoNeuTtsNanoSubsystem on the game thread.
 *   - The dtor waits for the worker thread to exit before returning.
 *   - Cancellation is cooperative: SignalCancel() sets an atomic the
 *     AR loop checks between sampler iterations.
 *
 * Milestone 3 skeleton:
 *   - Constructor starts the thread, Run() blocks waiting on the
 *     (empty) queue event. Stop() signals shutdown; dtor joins.
 *   - No actual synthesis path yet — Milestone 4 adds Enqueue + the
 *     full prompt → decode → sample → codec → PCM pipeline.
 */
class FInoNeuTtsNanoSynthesisWorker : public FRunnable
{
public:
    FInoNeuTtsNanoSynthesisWorker(
        TWeakObjectPtr<UInoNeuTtsNanoSubsystem> InOwner,
        FInoNeuTtsNanoRunner* InRunner,
        const FInoNeuTtsNanoVoiceRegistry* InVoiceRegistry);

    virtual ~FInoNeuTtsNanoSynthesisWorker();

    FInoNeuTtsNanoSynthesisWorker(const FInoNeuTtsNanoSynthesisWorker&) = delete;
    FInoNeuTtsNanoSynthesisWorker& operator=(const FInoNeuTtsNanoSynthesisWorker&) = delete;

    //~ FRunnable
    virtual uint32 Run() override;
    virtual void   Stop() override;
    //~ End FRunnable

    /** Cooperative cancel: the next cancel-check point inside the AR
     *  loop will abandon the current synthesis. Safe to call from any
     *  thread. */
    void SignalCancel();

    /** Game-thread entry point — copies the pending synth into the
     *  queue and wakes the worker. Consumes one Spsc slot; serialised
     *  in the subsystem (never called from multiple threads). */
    void Enqueue(FInoNeuTtsNanoPendingSynth Pending);

private:
    // Non-owning references. The subsystem guarantees these outlive
    // the worker (dtor order: worker first via Worker.Reset(), then
    // Runner.Reset(), then VoiceRegistry.Reset()).
    TWeakObjectPtr<UInoNeuTtsNanoSubsystem> WeakSubsystem;
    FInoNeuTtsNanoRunner*                   Runner        = nullptr;
    const FInoNeuTtsNanoVoiceRegistry*      VoiceRegistry = nullptr;

    // Thread + queue-signalling event.
    FRunnableThread* Thread     = nullptr;
    FEvent*          QueueEvent = nullptr;   // triggered by Enqueue + Stop()

    // Lifecycle atomics.
    TAtomic<bool> bStopRequested{false};
    TAtomic<bool> bStreamCancelled{false};

    // Spsc producer: subsystem game-thread Enqueue. Consumer: Run().
    TQueue<FInoNeuTtsNanoPendingSynth, EQueueMode::Spsc> Queue;

    // Core synthesis pipeline — runs on the worker thread.
    void ProcessSynth(FInoNeuTtsNanoPendingSynth& Pending);

    // Game-thread marshal — dispatches OnComplete via AsyncTask so the
    // dynamic delegate fires on the correct thread.
    void DispatchCompleteOnGameThread(
        FOnInoNeuTtsNanoSynthesisComplete OnComplete,
        bool             bSuccess,
        TArray<uint8>    PcmInt16LE,
        int32            SampleRate,
        FString          ErrorMessage);
};
