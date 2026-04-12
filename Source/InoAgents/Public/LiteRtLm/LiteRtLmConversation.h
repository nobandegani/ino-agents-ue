// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "LiteRtLm/LiteRtLmTypes.h"

#include "LiteRtLmConversation.generated.h"

class ULiteRtLmSubsystem;
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
 * Current surface: SendMessageAsync (streaming via
 * litert_lm_conversation_send_message_stream on the worker), OnToken
 * (per-chunk text delta), OnComplete (once at end with full
 * accumulated text from the final round), OnError (once on failure),
 * OnToolCalled (diagnostic, fires after each tool round-trip),
 * Cancel, Shutdown, SubmitDeferredToolResult (stubbed for future).
 *
 * Per-send ordering guarantee: OnToken fires zero or more times on
 * the game thread, in order, for text chunks from the FINAL round
 * of the agent loop only (tokens emitted during intermediate
 * tool-call rounds are suppressed — the caller never sees the
 * model's tool-call JSON as OnToken). OnToolCalled fires zero or
 * more times on the game thread for each executed tool call, in
 * order. After all rounds complete, exactly one of OnComplete or
 * OnError fires, always strictly AFTER every OnToken / OnToolCalled
 * broadcast for the same send. A caller that only wants the final
 * answer can ignore OnToken and OnToolCalled and read the text
 * passed to OnComplete.
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
    // Entry points
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

    /** Strip all [bracketed] tags from a string.
     *  "[cheerfully] Hello!" → "Hello!" */
    UFUNCTION(BlueprintPure, Category="InoAgents|LiteRT-LM")
    static FString StripTags(const FString& Raw);

    // ------------------------------------------------------------------
    // History
    // ------------------------------------------------------------------

    /** Get the full conversation history (user + assistant messages). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category="InoAgents|LiteRT-LM")
    const TArray<FLiteRtLmMessage>& GetHistory() const { return History; }

    /** Clear the tracked history. Does NOT affect the native conversation's
     *  KV cache — the model still remembers prior turns. This only clears
     *  the UE-side record used for save/load. */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void ClearHistory() { History.Reset(); }

    // ------------------------------------------------------------------
    // Context (per-turn injection, not stored in chat history)
    // ------------------------------------------------------------------

    /** Set a system context value (game/world state). */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Context")
    void SetSystemContext(const FString& Key, const FString& Value);

    /** Set a user context value (player state). */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Context")
    void SetUserContext(const FString& Key, const FString& Value);

    /** Add a system context value. Same as SetSystemContext. */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Context")
    void AddSystemContext(const FString& Key, const FString& Value);

    /** Add a user context value. Same as SetUserContext. */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Context")
    void AddUserContext(const FString& Key, const FString& Value);

    /** Get a system context value. Empty if not found. */
    UFUNCTION(BlueprintPure, Category="InoAgents|LiteRT-LM|Context")
    FString GetSystemContext(const FString& Key) const;

    /** Get a user context value. Empty if not found. */
    UFUNCTION(BlueprintPure, Category="InoAgents|LiteRT-LM|Context")
    FString GetUserContext(const FString& Key) const;

    /** Clear all system context. */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Context")
    void ClearSystemContext();

    /** Clear all user context. */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM|Context")
    void ClearUserContext();

    /**
     * Cancel the in-flight stream, if any. Safe to call at any time
     * from the game thread; safe to call with no stream in flight
     * (no-op). After Cancel, the currently-running send will emit
     * OnError("Cancelled by caller") on the game thread once the
     * native cancel has propagated through LiteRT-LM (typically
     * within a few hundred milliseconds).
     *
     * Cancel does NOT drain the queue — any messages that have been
     * enqueued but not yet started will still run. To abort everything
     * AND release resources, call Shutdown instead.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void Cancel();

    /**
     * True while a SendMessageAsync is actively generating — i.e.
     * from the moment the worker's stream has been kicked off to
     * the moment the round's terminal callback fires. Useful for
     * Blueprint UI that wants to disable the "Send" button while
     * the model is replying, or to show a typing indicator.
     *
     * Returns false before the first send, between rounds of the
     * agent loop (briefly), after OnComplete / OnError has fired,
     * and after Shutdown has been called.
     *
     * Safe to call from any thread, but the intended caller is the
     * game thread from Blueprint / Tick.
     */
    UFUNCTION(BlueprintPure, Category="InoAgents|LiteRT-LM")
    bool IsStreamingInFlight() const;

    /**
     * Immediately release the worker thread and native LiteRT-LM
     * resources. After calling Shutdown the conversation is a
     * "zombie": SendMessageAsync will log an error and fail, Cancel
     * becomes a no-op, and no further delegate broadcasts will fire.
     * The UObject itself remains alive until natural garbage
     * collection reclaims it.
     *
     * Intended for callers that need DETERMINISTIC teardown without
     * waiting for GC — for example:
     *   - Tests that want native resources released before the next
     *     assertion / test run.
     *   - Scene transitions that need the LiteRT-LM engine available
     *     for a new conversation on the next frame.
     *   - Manual lifetime control in C++ code that can't tolerate
     *     GC latency.
     *
     * In normal Blueprint gameplay you usually do NOT need to call
     * this — just drop the last reference to the conversation and
     * let GC handle it. The worker + native teardown happens during
     * BeginDestroy, which is safe in gameplay because the last
     * reference drop typically happens outside of any delegate
     * broadcast.
     *
     * Safe to call multiple times (second call is a no-op). Safe to
     * call while a stream is in flight — the destructor cancels the
     * stream and joins the worker thread before returning, same as
     * BeginDestroy.
     *
     * IMPORTANT: calling Shutdown from inside one of the
     * conversation's own delegate handlers (OnComplete, OnError,
     * OnToken) is supported — the internal Worker.Reset() does not
     * touch the delegate invocation list and is immune to the
     * reentrancy issues that make CollectGarbage() unsafe in the
     * same context.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void Shutdown();

    /**
     * Advanced: submit a tool result asynchronously, after the
     * conversation worker has already returned control to the game
     * thread waiting for it. Reserved for a future iteration where
     * tool implementations need to do their own async work (network
     * calls, disk I/O, user confirmation dialogs) before answering.
     *
     * STUBBED — all tool calls are currently executed synchronously
     * on the game thread inside the conversation's agent loop, which
     * blocks the worker thread until Execute returns. Calling this
     * method logs a warning and is otherwise a no-op.
     *
     * The method exists in the header now so that consumers of the
     * plugin can already wire it up in Blueprint — a future commit
     * will add the worker-side state machine that actually consumes
     * the deferred result.
     */
    UFUNCTION(BlueprintCallable, Category="InoAgents|LiteRT-LM")
    void SubmitDeferredToolResult(FName ToolCallId, const FString& ResultJson);

    // ------------------------------------------------------------------
    // Delegates (multicast, Blueprint-bindable)
    // ------------------------------------------------------------------

    /**
     * Fires zero or more times per SendMessageAsync call as the model
     * streams out its response. Each broadcast delivers two strings:
     *
     *   RawText   — the chunk as the model produced it, including any
     *               [emotion] or [audio] tags.
     *   CleanText — the same chunk with all [bracketed] tags stripped.
     *
     * Always on the game thread, in order.
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

    /**
     * Fires each time the accumulated streaming tokens cross a newline
     * boundary (\n). Delivers two strings:
     *   RawText   — the line with [emotion] / [audio] tags intact
     *               (send to ElevenLabs for expressive delivery)
     *   CleanText — the line with all [bracketed] tags stripped
     *               (use for subtitles, chat display, etc.)
     *
     * At OnComplete time, any remaining text that hasn't crossed a
     * newline is flushed as a final OnSentence broadcast, so the
     * concatenation of every OnSentence always equals the full
     * assistant response.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnLiteRtLmSentence OnSentence;

    /**
     * Fires at each newline boundary, right after the corresponding
     * OnSentence broadcast. Wire this to
     * UInoAgentsLiteRtLmDialogueQueue::EnqueuePause to insert timed silence
     * between audio segments.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnLiteRtLmNewLine OnNewLine;

    /**
     * Diagnostic event: fires AFTER a tool has been executed and its
     * result has been fed back into the conversation. Broadcast on
     * the game thread with the tool name, the arguments JSON the
     * model supplied, and the result JSON the tool returned.
     *
     * This is purely observational, for debug UI, logs, and
     * validation assertions in smoke tests. The tool call itself is
     * handled transparently inside the conversation worker — you do
     * NOT need to bind OnToolCalled to make tool calls work. You
     * only bind it if you want visibility into which tools ran.
     *
     * Fires once per tool call executed during a single
     * SendMessageAsync (so zero or more times per send, depending on
     * whether the model decided to use a tool and how many rounds
     * the agent loop ran). All broadcasts fire strictly before the
     * terminal OnComplete / OnError broadcast for the same send.
     */
    UPROPERTY(BlueprintAssignable, Category="InoAgents|LiteRT-LM")
    FOnLiteRtLmToolCalled OnToolCalled;

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
        const FLiteRtLmModelConfig& InConfig,
        const TArray<FLiteRtLmMessage>& InInitialMessages = TArray<FLiteRtLmMessage>());

    // ------------------------------------------------------------------
    // Sentence detection (called from worker's game-thread token path)
    // ------------------------------------------------------------------

    /**
     * Append a token chunk and broadcast OnSentence for each complete
     * sentence boundary found. Called automatically from the worker's
     * game-thread OnToken dispatch — callers do NOT need to call this
     * themselves; it runs as part of the normal token flow.
     *
     * Game thread only.
     */
    void AccumulateTokenForSentence(const FString& Chunk);

    /**
     * Broadcast any remaining text in SentenceBuffer as a final sentence.
     * Called from the game-thread OnComplete dispatch so no trailing
     * text is lost.
     *
     * Game thread only.
     */
    void FlushSentenceBuffer();

    /** Record an assistant response in the tracked history. Called by
     *  the worker's DispatchCompleteOnGameThread, game thread only. */
    void RecordAssistantMessage(const FString& Text);

    /** Filter a raw token chunk through the bracket-tag state machine.
     *  Returns the clean portion (characters outside [tags]). Handles
     *  tags that span multiple tokens. Game thread only. */
    FString FilterCleanToken(const FString& RawChunk);

private:
    UPROPERTY()
    TWeakObjectPtr<ULiteRtLmSubsystem> Subsystem;

    /** Tracked conversation history for save/load. Appended to in
     *  SendMessageAsync (user) and the OnComplete handler (assistant).
     *  Seeded from InitialMessages in Initialize if provided. */
    UPROPERTY()
    TArray<FLiteRtLmMessage> History;

    /** Context key-value maps. Merged on each SendMessageAsync. */
    TMap<FString, FString> SystemContextMap;
    TMap<FString, FString> UserContextMap;
    FString BuildMergedContext() const;

    int32 TokenTagDepth = 0;       // [bracket] depth
    int32 TokenCurlyDepth = 0;     // {curly} depth

    /** Rolling buffer for sentence detection. Accumulates tokens until
     *  a sentence-ending delimiter is found, at which point the complete
     *  sentence is broadcast via OnSentence and the buffer shifts to
     *  whatever follows the delimiter. */
    FString SentenceBuffer;

    // Worker owns the pinned thread + native conversation + native config.
    // TUniquePtr because FLiteRtLmConversationWorker is a plain C++ class,
    // not a UObject. Destroyed when this UObject's BeginDestroy runs, which
    // joins the worker thread before releasing native resources.
    TUniquePtr<FLiteRtLmConversationWorker> Worker;
};
