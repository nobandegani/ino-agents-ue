// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"

#include "UObject/WeakObjectPtr.h"

#include "NeuTtsNanoNative/InoNeuTtsNanoNativeTypes.h"   // FInoNeuTtsNanoNativeSynthesisOptions, FOn*

// Forward-decls — the worker's .cpp pulls in the real types.
class FInoNeuTtsNanoNativeRunner;
class FInoNeuTtsNanoNativeVoiceRegistry;
class UInoNeuTtsNanoNativeSubsystem;
class FEvent;

/**
 * One queued synthesis request. Plain-struct, passed by value through
 * the Spsc queue. The dynamic delegates inside are UObject-aware
 * handles — it's safe to copy them into a TQueue entry and later
 * invoke from the game thread (via AsyncTask marshaling in the worker).
 *
 * One-shot vs streaming: a pending synth is "streaming" iff
 * bStreamingEnabled is true. One-shot path leaves OnAudioChunk unbound
 * and Options.StreamChunkTokens ignored; streaming path fires
 * OnAudioChunk repeatedly during the AR loop with delta waveforms and
 * fires OnComplete at the end with the full concatenated audio.
 */
struct FInoNeuTtsNanoNativePendingSynth
{
    FString PhonemesText;                              // pre-phonemized IPA
    FName   VoiceName;
    FInoNeuTtsNanoNativeSynthesisOptions     Options;
    FOnInoNeuTtsNanoNativeSynthesisComplete  OnComplete;     // dynamic — invoke on GT

    /** True when the subsystem's SynthesizeStreamAsync entry point was
     *  used. When false, OnAudioChunk is left default-constructed and
     *  never fired. When true, OnAudioChunk fires on the game thread
     *  for each incremental audio delta; cadence is controlled by
     *  Options.StreamChunkTokens. */
    bool bStreamingEnabled = false;

    /** Dynamic delegate for per-chunk audio deltas. Only consulted
     *  when bStreamingEnabled==true. FString / TArray params by const
     *  ref per UE BindDynamic rules (same as OnComplete). */
    FOnInoNeuTtsNanoNativeAudioChunk OnAudioChunk;
};

/**
 * Dedicated synthesis worker thread for UInoNeuTtsNanoNativeSubsystem.
 *
 * Single-thread-serializing consumer of:
 *   - llama_context (not thread-safe — only one decode at a time)
 *   - FInoOnnxSession (thread-safe for Run, but we serialise to
 *     keep cancellation semantics clean)
 *
 * Threading contract (Milestone 3 scope — expanded in Milestone 4):
 *   - One pinned FRunnableThread per worker instance.
 *   - Created + Stopped by UInoNeuTtsNanoNativeSubsystem on the game thread.
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
class FInoNeuTtsNanoNativeSynthesisWorker : public FRunnable
{
public:
    FInoNeuTtsNanoNativeSynthesisWorker(
        TWeakObjectPtr<UInoNeuTtsNanoNativeSubsystem> InOwner,
        FInoNeuTtsNanoNativeRunner* InRunner,
        const FInoNeuTtsNanoNativeVoiceRegistry* InVoiceRegistry);

    virtual ~FInoNeuTtsNanoNativeSynthesisWorker();

    FInoNeuTtsNanoNativeSynthesisWorker(const FInoNeuTtsNanoNativeSynthesisWorker&) = delete;
    FInoNeuTtsNanoNativeSynthesisWorker& operator=(const FInoNeuTtsNanoNativeSynthesisWorker&) = delete;

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
    void Enqueue(FInoNeuTtsNanoNativePendingSynth Pending);

private:
    // Non-owning references. The subsystem guarantees these outlive
    // the worker (dtor order: worker first via Worker.Reset(), then
    // Runner.Reset(), then VoiceRegistry.Reset()).
    TWeakObjectPtr<UInoNeuTtsNanoNativeSubsystem> WeakSubsystem;
    FInoNeuTtsNanoNativeRunner*                   Runner        = nullptr;
    const FInoNeuTtsNanoNativeVoiceRegistry*      VoiceRegistry = nullptr;

    // Thread + queue-signalling event.
    FRunnableThread* Thread     = nullptr;
    FEvent*          QueueEvent = nullptr;   // triggered by Enqueue + Stop()

    // Lifecycle atomics.
    TAtomic<bool> bStopRequested{false};
    TAtomic<bool> bStreamCancelled{false};

    // Spsc producer: subsystem game-thread Enqueue. Consumer: Run().
    TQueue<FInoNeuTtsNanoNativePendingSynth, EQueueMode::Spsc> Queue;

    // Core synthesis pipeline — runs on the worker thread.
    void ProcessSynth(FInoNeuTtsNanoNativePendingSynth& Pending);

    // Game-thread marshal — dispatches OnComplete via AsyncTask so the
    // dynamic delegate fires on the correct thread. No SampleRate
    // param: NeuTTS Nano's output rate is fixed at 24 kHz (see
    // UInoNeuTtsNanoNativeSubsystem::GetOutputSampleRate).
    void DispatchCompleteOnGameThread(
        FOnInoNeuTtsNanoNativeSynthesisComplete OnComplete,
        bool             bSuccess,
        TArray<uint8>    PcmInt16LE,
        FString          ErrorMessage);

    // Game-thread marshal for streaming delta broadcasts. Same pattern
    // as DispatchCompleteOnGameThread — copy the delegate handle +
    // move the bytes into the lambda, verify the subsystem is still
    // alive at invocation time.
    void DispatchAudioChunkOnGameThread(
        FOnInoNeuTtsNanoNativeAudioChunk OnAudioChunk,
        TArray<uint8>              AudioChunk,
        bool                       bIsFinal,
        int32                      NumSpeechIds);
};
