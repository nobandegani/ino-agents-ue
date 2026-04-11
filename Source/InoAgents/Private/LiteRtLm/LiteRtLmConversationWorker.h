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
 *     FEvent, constructs the FRunnableThread, and begins Run().
 *   - Run() executes on the worker thread. It consumes from MessageQueue,
 *     calls blocking litert_lm_conversation_send_message, and dispatches
 *     results back to the game thread via AsyncTask. It never touches
 *     UObjects directly.
 *   - EnqueueMessage is called on the game thread to add work.
 *   - Stop() is called on the game thread to signal the worker to exit.
 *   - Destruction happens on the game thread. ~FLiteRtLmConversationWorker
 *     sets the stop flag, triggers the queue event to wake the worker,
 *     calls Thread->WaitForCompletion to join, then destroys the native
 *     conversation and config in that order.
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

    //~ FRunnable interface
    virtual uint32 Run() override;
    virtual void Stop() override;
    //~ End FRunnable interface

private:
    /**
     * Process one user message from start to finish: build the JSON
     * message, call litert_lm_conversation_send_message, parse the
     * response, dispatch OnComplete or OnError back to the game thread.
     * Runs on the worker thread.
     */
    void ProcessMessage(const FString& UserText);

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

    // Set to true by Stop() or ~FLiteRtLmConversationWorker to signal
    // the worker's Run() loop to exit. Atomic because the game thread
    // writes and the worker thread reads.
    TAtomic<bool> bStopRequested{false};

    // The pinned worker thread. Created in the constructor; joined in
    // the destructor via WaitForCompletion.
    TUniquePtr<FRunnableThread> Thread;
};
