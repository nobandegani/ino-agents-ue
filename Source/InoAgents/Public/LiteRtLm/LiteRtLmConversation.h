// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "LiteRtLm/LiteRtLmTypes.h"

#include "LiteRtLmConversation.generated.h"

class ULiteRtLmSubsystem;
class ULiteRtLmModelConfig;
class FLiteRtLmConversationWorker;

// Forward declarations of opaque native types from LiteRT-LM's C API.
// We deliberately do NOT include "litert/lm/engine.h" here — that would pull
// the C API surface into every translation unit that uses conversations.
extern "C" {
    struct LiteRtLmEngine;
    struct LiteRtLmConversation;
    struct LiteRtLmConversationConfig;
}

/**
 * One stateful conversation with a LiteRT-LM model.
 *
 * Construction: via ULiteRtLmSubsystem::CreateConversation. Do NOT construct
 * directly with NewObject — the subsystem must populate the internal native
 * conversation + worker thread.
 *
 * Lifetime: owned by whoever holds a UPROPERTY reference to it. When the
 * last reference drops, UE garbage collection eventually calls BeginDestroy,
 * which joins the worker thread and destroys native resources. For
 * deterministic cleanup in tests, release the reference and call
 * CollectGarbage(RF_NoFlags, true) — but in normal gameplay, letting GC
 * handle it is fine.
 *
 * Threading: SendMessageAsync returns immediately. Generation happens on
 * a pinned worker thread owned by the conversation; chunks and final
 * responses marshal back to the game thread via AsyncTask before any
 * delegate broadcasts. Blueprint code only ever sees delegates firing on
 * the game thread — there is no thread-safety burden on callers.
 *
 * Milestone D.3 surface: SendMessageAsync (streaming via
 * litert_lm_conversation_send_message_stream on the worker), OnToken
 * (per-chunk), OnComplete (once at end with full accumulated text),
 * OnError (once on failure), Cancel. D.4 will add OnToolCalled and
 * SubmitDeferredToolResult.
 *
 * Per-send ordering guarantee: OnToken fires zero or more times on
 * the game thread, in order, followed by exactly one of OnComplete
 * (if the stream finished cleanly) or OnError (if it failed or was
 * cancelled). A caller that only wants the final string can ignore
 * OnToken and read the text passed to OnComplete — the worker
 * accumulates chunks itself.
 */
UCLASS(BlueprintType)
class INOAGENTS_API ULiteRtLmConversation : public UObject
{
    GENERATED_BODY()

public:
    // Out-of-line constructors / destructor. Required because the private
    // TUniquePtr<FLiteRtLmConversationWorker> member references a
    // forward-declared type: if any compiler-synthesized constructor or
    // destructor were emitted in the .gen.cpp (where only the forward
    // decl is visible), TDefaultDelete<FLiteRtLmConversationWorker>::
    // operator() would try to `delete` an incomplete type and fail with
    // C4150. UHT emits TWO implicit ctors in .gen.cpp — the default one
    // AND the FVTableHelper hot-reload helper — so BOTH must be declared
    // here and defined in LiteRtLmConversation.cpp (which #includes the
    // worker header), along with the destructor.
    ULiteRtLmConversation();
    ULiteRtLmConversation(FVTableHelper& Helper);
    virtual ~ULiteRtLmConversation();

    //~ UObject interface
    virtual void BeginDestroy() override;
    //~ End UObject interface

    // ------------------------------------------------------------------
    // Entry points (D.2)
    // ------------------------------------------------------------------

    /**
     * Send a user message to the conversation. Returns immediately.
     * As the model generates the response, OnToken fires zero or more
     * times on the game thread with each chunk. When the stream ends,
     * exactly one of OnComplete (success — full accumulated text) or
     * OnError (failure or cancellation) fires on the game thread.
     *
     * Calling SendMessageAsync while a previous send is still in flight
     * enqueues the new message — it will be processed once the current
     * one completes. Messages are FIFO.
     *
     * Internally uses litert_lm_conversation_send_message_stream on a
     * dedicated worker thread; each chunk marshals back to the game
     * thread via AsyncTask before any delegate broadcasts. Callers in
     * Blueprint or C++ never see off-thread delegates.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void SendMessageAsync(const FString& UserText);

    /**
     * Cancel the in-flight stream, if any. Safe to call at any time
     * from the game thread; safe to call with no stream in flight
     * (no-op). After Cancel, the currently-running send will emit
     * OnError("Cancelled by caller") on the game thread once the
     * native cancel has propagated through LiteRT-LM (typically
     * within a few hundred milliseconds).
     *
     * Cancel does NOT drain the queue — any messages that have been
     * enqueued but not yet started will still run. To abort everything,
     * destroy the conversation (release the last reference so it GCs).
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void Cancel();

    // ------------------------------------------------------------------
    // Delegates (multicast, Blueprint-bindable)
    // ------------------------------------------------------------------

    /**
     * Fires zero or more times per SendMessageAsync call as the model
     * streams out its response. Each broadcast delivers one chunk of
     * assistant text, in order. Always on the game thread. The
     * accumulated concatenation of every Chunk that OnToken emits for
     * a single send is identical to the FullText that OnComplete
     * delivers at the end — callers only need to listen to one or
     * the other, not both.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnLiteRtLmToken OnToken;

    /**
     * Fires exactly once per SendMessageAsync call on success. FullText
     * is the assistant's response concatenated from every content part
     * of type="text". Always on the game thread. Always fires AFTER
     * every OnToken broadcast for the same send — the worker orders
     * the AsyncTask dispatches so the game-thread observer sees
     * tokens in order and then the completion.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnLiteRtLmComplete OnComplete;

    /**
     * Fires exactly once per SendMessageAsync call on failure. Mutually
     * exclusive with OnComplete — exactly one of them fires per send.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnLiteRtLmError OnError;

    // ------------------------------------------------------------------
    // Internal — called by ULiteRtLmSubsystem::CreateConversation only
    // ------------------------------------------------------------------

    /**
     * Set up native resources and spawn the worker thread. Called exactly
     * once per instance, by the subsystem's CreateConversation factory.
     * Game thread only.
     */
    void Initialize(
        ULiteRtLmSubsystem* InSubsystem,
        LiteRtLmEngine* InEngine,
        const ULiteRtLmModelConfig* InConfig);

private:
    UPROPERTY()
    TWeakObjectPtr<ULiteRtLmSubsystem> Subsystem;

    // Worker owns the pinned thread + native conversation + native config.
    // TUniquePtr because FLiteRtLmConversationWorker is a plain C++ class,
    // not a UObject. Destroyed when this UObject's BeginDestroy runs, which
    // joins the worker thread before releasing native resources.
    TUniquePtr<FLiteRtLmConversationWorker> Worker;
};
