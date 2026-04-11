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

class ULiteRtLmConversation;
class FRunnableThread;
class FEvent;

/**
 * FRunnable that owns one native LiteRtLmConversation and processes
 * SendMessage requests on its own dedicated pinned thread.
 *
 * This is NOT a UObject. It is a plain C++ class held in a
 * TUniquePtr<FLiteRtLmConversationWorker> by ULiteRtLmConversation.
 * It must not be shared — one ULiteRtLmConversation owns exactly one
 * FLiteRtLmConversationWorker.
 *
 * Threading contract:
 *   - Construction happens on the game thread. Takes ownership of the
 *     native conversation + config pointers passed in. Creates the
 *     queue and stream FEvents, constructs the FRunnableThread, and
 *     begins Run().
 *   - Run() executes on the worker thread. It consumes from MessageQueue,
 *     calls litert_lm_conversation_send_message_stream, and blocks on
 *     StreamEvent until the native stream callback reports is_final or
 *     an error. The static C callback runs on LiteRT-LM's internal
 *     thread (NOT our worker thread); it copies each chunk, dispatches
 *     an OnToken broadcast to the game thread via AsyncTask, and on
 *     the final chunk signals StreamEvent so our worker thread wakes
 *     up and dispatches OnComplete (or OnError).
 *   - EnqueueMessage is called on the game thread to add work.
 *   - Cancel is called on the game thread to abort the in-flight stream.
 *     It sets an atomic flag and calls litert_lm_conversation_cancel_process,
 *     which causes LiteRT-LM to fire a final callback shortly thereafter.
 *     The worker thread then dispatches OnError("Cancelled by caller").
 *   - Stop() is called on the game thread to signal the worker to exit.
 *   - Destruction happens on the game thread. ~FLiteRtLmConversationWorker
 *     sets the stop flag, cancels any in-flight stream, triggers the
 *     queue event to wake the worker if idle, calls Thread->WaitForCompletion
 *     to join, then destroys the native conversation and config in that
 *     order. Waiting for the thread to finish also waits for LiteRT-LM's
 *     final callback to fire (because the worker is blocked inside
 *     ProcessMessage's StreamEvent->Wait) — by the time we touch native
 *     pointers we are guaranteed the C API is done calling us back.
 *
 * The class holds a TWeakObjectPtr<ULiteRtLmConversation> for marshaling
 * results back to the owning UObject. The weak pointer is captured by
 * value into AsyncTask lambdas; the game-thread lambda checks validity
 * before dereferencing. If the UObject has been GC'd by the time the
 * lambda runs, the broadcast is skipped and no use-after-free occurs.
 */
class FLiteRtLmConversationWorker : public FRunnable
{
public:
    /**
     * Construct on the game thread. Takes ownership of InConversation and
     * InConversationConfig — the destructor will call litert_lm_* delete
     * on both. Creates and starts the worker thread.
     *
     * InOwner must be a valid weak pointer to the ULiteRtLmConversation
     * that owns this worker. It is captured and used only to dispatch
     * delegate broadcasts back to the game thread.
     */
    FLiteRtLmConversationWorker(
        TWeakObjectPtr<ULiteRtLmConversation> InOwner,
        LiteRtLmConversation* InConversation,
        LiteRtLmConversationConfig* InConversationConfig);

    virtual ~FLiteRtLmConversationWorker();

    // Non-copyable, non-movable — the worker owns a thread and native
    // pointers; copying or moving would be a disaster.
    FLiteRtLmConversationWorker(const FLiteRtLmConversationWorker&) = delete;
    FLiteRtLmConversationWorker& operator=(const FLiteRtLmConversationWorker&) = delete;
    FLiteRtLmConversationWorker(FLiteRtLmConversationWorker&&) = delete;
    FLiteRtLmConversationWorker& operator=(FLiteRtLmConversationWorker&&) = delete;

    /**
     * Add a user message to the worker's queue. Thread-safe: called from
     * the game thread; the worker thread consumes from the same queue.
     * Triggers the queue event to wake the worker if it was idle.
     */
    void EnqueueMessage(FString UserText);

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
     * Process one user message from start to finish: build the JSON
     * message, kick off litert_lm_conversation_send_message_stream,
     * block on StreamEvent until the native stream callback reports
     * is_final or an error, then dispatch OnComplete or OnError back
     * to the game thread. Runs on the worker thread.
     */
    void ProcessMessage(const FString& UserText);

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
     * OnToken broadcasts to the game thread via AsyncTask, and on
     * the final chunk populates StreamError / StreamAccumulated and
     * signals StreamEvent so the worker thread wakes up. Never
     * touches UObject state directly.
     */
    void OnStreamChunk(const char* chunk, bool is_final, const char* error_msg);

    /** Dispatch an OnToken(Chunk) broadcast to the game thread. */
    void DispatchTokenOnGameThread(FString Chunk);

    /** Dispatch an OnComplete(FullText) broadcast to the game thread. */
    void DispatchCompleteOnGameThread(FString FullText);

    /** Dispatch an OnError(ErrorMessage) broadcast to the game thread. */
    void DispatchErrorOnGameThread(FString ErrorMessage);

    // Weak reference to the UObject that owns this worker. Captured by
    // value into game-thread lambdas. Do NOT .Get() from the worker
    // thread; dereferencing is only valid on the game thread.
    TWeakObjectPtr<ULiteRtLmConversation> WeakOwner;

    // Native LiteRT-LM resources. Owned by this worker from construction
    // to destructor. Destroyed in reverse order of creation (conversation
    // first, then config) in ~FLiteRtLmConversationWorker.
    LiteRtLmConversation*       NativeConversation       = nullptr;
    LiteRtLmConversationConfig* NativeConversationConfig = nullptr;

    // SPSC queue: game thread produces, worker thread consumes.
    TQueue<FString, EQueueMode::Spsc> MessageQueue;

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

    // Set to true by Stop() or ~FLiteRtLmConversationWorker to signal
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

    // Accumulated assistant text, written only by OnStreamChunk on
    // LiteRT-LM's internal thread; read only by the worker thread
    // after StreamEvent unblocks. The stream-event synchronization
    // provides the happens-before edge — no separate lock needed.
    FString StreamAccumulated;

    // Error message captured by OnStreamChunk if LiteRT-LM reports
    // error_msg != nullptr. Empty on success. Read by the worker
    // thread after StreamEvent unblocks.
    FString StreamError;

    // The pinned worker thread. Created in the constructor; joined in
    // the destructor via WaitForCompletion.
    TUniquePtr<FRunnableThread> Thread;
};
