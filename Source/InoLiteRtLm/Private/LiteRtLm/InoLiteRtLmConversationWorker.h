// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "Templates/Atomic.h"
#include "Templates/UniquePtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

// Forward declarations of LiteRT-LM opaque types.
extern "C" {
    struct LiteRtLmConversation;
    struct LiteRtLmConversationConfig;
}

class UInoLiteRtLmConversation;
class UInoLiteRtLmSubsystem;
class FRunnableThread;
class FEvent;

/**
 * FRunnable that owns one native LiteRtLmConversation and processes
 * SendMessage requests on its own dedicated pinned thread.
 *
 * This is NOT a UObject. It is a plain C++ class held in a
 * TUniquePtr<FInoLiteRtLmConversationWorker> by UInoLiteRtLmConversation.
 * It must not be shared — one UInoLiteRtLmConversation owns exactly one
 * FInoLiteRtLmConversationWorker.
 *
 * Threading contract:
 *   - Construction happens on the game thread. Takes ownership of the
 *     native conversation + config pointers passed in. Creates the
 *     queue and stream FEvents, constructs the FRunnableThread, and
 *     begins Run().
 *   - Run() executes on the worker thread. It consumes from MessageQueue
 *     and calls ProcessMessage for each entry.
 *   - ProcessMessage runs a multi-round agent loop on the worker thread:
 *       round 0:   send the user message
 *       round N+1: if round N produced tool calls, execute them on the
 *                  game thread, build a tool_result message, and send
 *                  that via a fresh send_message_stream call on the
 *                  same native conversation (same KV cache)
 *       terminate: when a round produces no tool calls, its accumulated
 *                  text is the final answer and dispatches OnComplete
 *     Each round reuses the same static stream callback
 *     (OnStreamChunkStatic). The callback runs on LiteRT-LM's internal
 *     thread — not our worker thread — and dispatches per-chunk text
 *     via OnToken to the game thread via AsyncTask, while quietly
 *     collecting tool_call parts into StreamPendingToolCalls for the
 *     worker thread to pick up after StreamEvent unblocks.
 *   - EnqueueMessage is called on the game thread to add work.
 *   - Cancel is called on the game thread to abort the in-flight stream.
 *     It sets an atomic flag and calls litert_lm_conversation_cancel_process,
 *     which causes LiteRT-LM to fire a final callback shortly thereafter.
 *     The worker thread then dispatches OnError("Cancelled by caller").
 *   - Stop() is called on the game thread to signal the worker to exit.
 *   - Destruction happens on the game thread. ~FInoLiteRtLmConversationWorker
 *     sets the stop flag, cancels any in-flight stream, triggers the
 *     queue event to wake the worker if idle, calls Thread->WaitForCompletion
 *     to join, then destroys the native conversation and config in that
 *     order. Waiting for the thread to finish also waits for LiteRT-LM's
 *     final callback to fire (because the worker is blocked inside
 *     ProcessMessage's StreamEvent->Wait) — by the time we touch native
 *     pointers we are guaranteed the C API is done calling us back.
 *
 * The class holds a TWeakObjectPtr<UInoLiteRtLmConversation> for marshaling
 * results back to the owning UObject. The weak pointer is captured by
 * value into AsyncTask lambdas; the game-thread lambda checks validity
 * before dereferencing. If the UObject has been GC'd by the time the
 * lambda runs, the broadcast is skipped and no use-after-free occurs.
 */
class FInoLiteRtLmConversationWorker : public FRunnable
{
public:
    /**
     * Construct on the game thread. Takes ownership of InConversation and
     * InConversationConfig — the destructor will call litert_lm_* delete
     * on both. Creates and starts the worker thread.
     *
     * InOwner must be a valid weak pointer to the UInoLiteRtLmConversation
     * that owns this worker. It is captured and used only to dispatch
     * delegate broadcasts back to the game thread.
     */
    FInoLiteRtLmConversationWorker(
        TWeakObjectPtr<UInoLiteRtLmConversation> InOwner,
        TWeakObjectPtr<UInoLiteRtLmSubsystem>    InSubsystem,
        LiteRtLmConversation* InConversation,
        LiteRtLmConversationConfig* InConversationConfig);

    virtual ~FInoLiteRtLmConversationWorker();

    // Non-copyable, non-movable — the worker owns a thread and native
    // pointers; copying or moving would be a disaster.
    FInoLiteRtLmConversationWorker(const FInoLiteRtLmConversationWorker&) = delete;
    FInoLiteRtLmConversationWorker& operator=(const FInoLiteRtLmConversationWorker&) = delete;
    FInoLiteRtLmConversationWorker(FInoLiteRtLmConversationWorker&&) = delete;
    FInoLiteRtLmConversationWorker& operator=(FInoLiteRtLmConversationWorker&&) = delete;

    /**
     * Add a user message to the worker's queue. Thread-safe: called from
     * the game thread; the worker thread consumes from the same queue.
     * Triggers the queue event to wake the worker if it was idle.
     */
    void EnqueueMessage(FString UserText, FString ExtraContext = FString());

    /**
     * Snapshot of the in-flight flag. Reads the atomic so it is safe
     * to call from any thread, though the intended caller is
     * UInoLiteRtLmConversation::IsStreamingInFlight on the game thread.
     * Returns true from the moment the worker calls
     * litert_lm_conversation_send_message_stream until the round's
     * final callback signals StreamEvent and the worker clears the
     * flag. Between rounds of a multi-round agent loop it briefly
     * reads as false, which is fine for the usual "disable send
     * button while a reply is in progress" use case.
     */
    bool IsStreamInFlight() const { return bStreamInFlight.Load(); }

    /**
     * Cancel the in-flight stream, if any. Called on the game thread.
     * Sets an atomic cancel flag and invokes
     * litert_lm_conversation_cancel_process on the native conversation.
     * LiteRT-LM will fire its final stream callback shortly thereafter,
     * which unblocks the worker thread's StreamEvent wait and causes
     * it to dispatch OnError("Cancelled by caller").
     *
     * Safe to call with no stream in flight (no-op). Safe to call from
     * any thread, though the public API only ever calls it from the
     * game thread.
     */
    void Cancel();

    //~ FRunnable interface
    virtual uint32 Run() override;
    virtual void Stop() override;
    //~ End FRunnable interface

private:
    /**
     * One pending tool call collected from the current round's stream.
     * Built by OnStreamChunk on the LiteRT-LM callback thread,
     * consumed by ProcessMessage on the worker thread after
     * StreamEvent unblocks.
     */
    struct FPendingToolCall
    {
        FName   Name;
        FString ArgumentsJson;  // object, serialised
    };

    /**
     * Process one user message from start to finish: runs the
     * multi-round agent loop described in the class header. Each
     * round issues a fresh litert_lm_conversation_send_message_stream
     * call on the same native conversation, blocks on StreamEvent
     * until is_final/error, and then either executes collected tool
     * calls and loops to the next round, or dispatches OnComplete
     * with the final text. Runs on the worker thread.
     */
    void ProcessMessage(const FString& UserText, const FString& ExtraContext);

    /**
     * Run ONE round of the agent loop: reset per-round state, call
     * send_message_stream with the given pre-built JSON message,
     * wait for the stream to finish, and return. The caller (the
     * outer ProcessMessage loop) inspects StreamPendingToolCalls,
     * StreamError, StreamAccumulated, and bStreamCancelled after
     * this returns to decide what to do next. Returns true if the
     * round ran the stream successfully (regardless of whether it
     * produced tool calls or terminal text); returns false if the
     * stream failed to start (in which case StreamError is
     * populated and the caller should dispatch OnError).
     */
    bool RunOneStreamRound(const FString& MessageJson,
                           const FString& ExtraContextForRound);

    /**
     * Execute a single tool call on the game thread and return the
     * tool's result JSON. Runs on the worker thread; internally
     * queues an AsyncTask to the game thread, blocks on an FEvent
     * until the task fulfils the result, and returns. If the
     * subsystem has been GC'd or the tool is not registered, returns
     * a JSON-formatted error string that the caller forwards to
     * the model verbatim.
     */
    FString ExecuteToolSynchronously(const FPendingToolCall& Call);

    /**
     * Static C-callable trampoline passed to LiteRT-LM as the stream
     * callback. Receives the worker instance via callback_data and
     * forwards to OnStreamChunk. Runs on LiteRT-LM's internal thread,
     * NOT on our worker thread.
     */
    static void OnStreamChunkStatic(
        void* callback_data,
        const char* chunk,
        bool is_final,
        const char* error_msg);

    /**
     * Instance method invoked by OnStreamChunkStatic. Runs on
     * LiteRT-LM's internal thread. Copies chunk/error strings (they
     * are valid only for the duration of this call), dispatches
     * OnToken broadcasts to the game thread via AsyncTask for text
     * parts, collects tool_call parts into StreamPendingToolCalls,
     * and on the final chunk populates StreamError / StreamAccumulated
     * and signals StreamEvent so the worker thread wakes up. Never
     * touches UObject state directly.
     */
    void OnStreamChunk(const char* chunk, bool is_final, const char* error_msg);

    /** Dispatch an OnToken(Chunk) broadcast to the game thread. */
    void DispatchTokenOnGameThread(FString Chunk);

    /** Dispatch an OnComplete(FullText) broadcast to the game thread. */
    void DispatchCompleteOnGameThread(FString FullText);

    /** Dispatch an OnError(ErrorMessage) broadcast to the game thread. */
    void DispatchErrorOnGameThread(FString ErrorMessage);

    /**
     * Dispatch an OnToolCalled(ToolName, ArgumentsJson, ResultJson)
     * broadcast to the game thread. Called once per tool executed in
     * ExecuteToolSynchronously, strictly before OnComplete for the
     * overall send.
     */
    void DispatchToolCalledOnGameThread(
        FName ToolName, FString ArgumentsJson, FString ResultJson);

    // Weak reference to the UObject that owns this worker. Captured by
    // value into game-thread lambdas. Do NOT .Get() from the worker
    // thread; dereferencing is only valid on the game thread.
    TWeakObjectPtr<UInoLiteRtLmConversation> WeakOwner;

    // Weak reference to the subsystem, used by the agent loop's
    // game-thread tool execution task to look up tools in the
    // registry. Same rules as WeakOwner — dereference only from
    // game-thread lambdas, never from the worker thread directly.
    TWeakObjectPtr<UInoLiteRtLmSubsystem> WeakSubsystem;

    // Native LiteRT-LM resources. Owned by this worker from construction
    // to destructor. Destroyed in reverse order of creation (conversation
    // first, then config) in ~FInoLiteRtLmConversationWorker.
    LiteRtLmConversation*       NativeConversation       = nullptr;
    LiteRtLmConversationConfig* NativeConversationConfig = nullptr;

    struct FPendingMessage
    {
        FString UserText;
        FString ExtraContext;
    };

    // SPSC queue: game thread produces, worker thread consumes.
    TQueue<FPendingMessage, EQueueMode::Spsc> MessageQueue;

    // Event used to wake the worker when a new message is enqueued or
    // when Stop() is called. Created from the UE event pool; returned
    // to the pool in the destructor.
    FEvent* QueueEvent = nullptr;

    // Event used by the static stream callback to signal the worker
    // thread that the current stream has finished (is_final or error).
    // Created manual-reset so we can safely reset it before each send
    // and know subsequent Triggers won't be lost. Returned to the pool
    // in the destructor after the worker thread has joined.
    FEvent* StreamEvent = nullptr;

    // Set to true by Stop() or ~FInoLiteRtLmConversationWorker to signal
    // the worker's Run() loop to exit. Atomic because the game thread
    // writes and the worker thread reads.
    TAtomic<bool> bStopRequested{false};

    // True from the moment the worker calls
    // litert_lm_conversation_send_message_stream until StreamEvent is
    // signaled by the final callback. Used by Cancel() and by the
    // destructor to decide whether to call the native cancel API.
    // Atomic because Cancel() runs on the game thread while the flag
    // is written by the worker thread.
    TAtomic<bool> bStreamInFlight{false};

    // Set by Cancel() on the game thread; read by the worker thread
    // after StreamEvent unblocks, and by OnStreamChunk on LiteRT-LM's
    // internal thread (the callback still needs to deliver the final
    // chunk and signal StreamEvent, but subsequent game-thread token
    // dispatches are suppressed to keep the stream ordering clean).
    // Cleared by the worker thread at the start of each ProcessMessage.
    TAtomic<bool> bStreamCancelled{false};

    // Accumulated assistant text for the CURRENT round, written only
    // by OnStreamChunk on LiteRT-LM's internal thread; read only by
    // the worker thread after StreamEvent unblocks. The stream-event
    // synchronization provides the happens-before edge — no separate
    // lock needed. Cleared at the top of every round by
    // RunOneStreamRound so only the final round's text reaches
    // OnComplete; tool-call rounds don't leak their text through.
    FString StreamAccumulated;

    // Collected tool_call parts for the CURRENT round, written only
    // by OnStreamChunk on the LiteRT-LM callback thread, read only
    // by the worker thread after StreamEvent unblocks. Empty means
    // the round was terminal (final text). Non-empty means the
    // agent loop should execute these and send a tool_result to
    // start the next round. Cleared at the top of every round.
    TArray<FPendingToolCall> StreamPendingToolCalls;

    // Error message captured by OnStreamChunk if LiteRT-LM reports
    // error_msg != nullptr. Empty on success. Read by the worker
    // thread after StreamEvent unblocks.
    FString StreamError;

    // The pinned worker thread. Created in the constructor; joined in
    // the destructor via WaitForCompletion.
    TUniquePtr<FRunnableThread> Thread;
};
