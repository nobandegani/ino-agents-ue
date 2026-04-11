// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLmConversationWorker.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmConversation.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"  // EscapeJsonString

#include "litert/lm/engine.h"

FLiteRtLmConversationWorker::FLiteRtLmConversationWorker(
    TWeakObjectPtr<ULiteRtLmConversation> InOwner,
    LiteRtLmConversation* InConversation,
    LiteRtLmConversationConfig* InConversationConfig)
    : WeakOwner(InOwner)
    , NativeConversation(InConversation)
    , NativeConversationConfig(InConversationConfig)
{
    // QueueEvent is auto-reset: one Trigger wakes the worker exactly
    // once, matching "one enqueue = one wake, one Stop = one wake".
    QueueEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/ false);

    // StreamEvent is manual-reset: before each send, the worker thread
    // calls ->Reset() to clear any stale signal, then Wait()s. The
    // callback thread signals it exactly once on is_final or error.
    // Manual-reset (rather than auto) is safer here because there is
    // ONE waiter and (potentially) ONE signaler per send, and Reset
    // before each Wait guarantees we never see a leftover trigger
    // from the previous message.
    StreamEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/ true);

    // Start the thread. TPri_Normal is fine for LLM inference —
    // bumping priority can starve the game thread on single-core
    // laptops and rarely helps on modern multicore machines.
    Thread.Reset(FRunnableThread::Create(
        this,
        TEXT("LiteRtLmConversationWorker"),
        /*InStackSize=*/ 0,
        TPri_Normal));

    UE_LOG(LogInoAgents, Log,
           TEXT("FLiteRtLmConversationWorker: thread started"));
}

FLiteRtLmConversationWorker::~FLiteRtLmConversationWorker()
{
    // Signal the worker to exit its Run() loop. This is the path for a
    // graceful shutdown from the game thread.
    bStopRequested = true;

    // If a stream is in flight, we must cancel it so the LiteRT-LM
    // callback fires its final chunk and signals StreamEvent. Otherwise
    // the worker thread is stuck inside ProcessMessage's StreamEvent->
    // Wait and will never see bStopRequested, and Thread->WaitForCompletion
    // below will hang forever.
    //
    // Setting bStreamCancelled first ensures that when the worker wakes
    // up from StreamEvent->Wait, it dispatches OnError("Cancelled")
    // rather than OnComplete(partial text). That error will fire on the
    // game thread AFTER this destructor returns, but the AsyncTask
    // broadcast checks the weak pointer — if the owning ULiteRtLmConversation
    // has already been GC'd, the broadcast is skipped.
    if (bStreamInFlight.Load())
    {
        bStreamCancelled = true;
        if (NativeConversation != nullptr)
        {
            litert_lm_conversation_cancel_process(NativeConversation);
        }
    }

    // Wake the queue-wait event in case the worker is idle between
    // messages. If it's mid-stream instead, this is a no-op for the
    // worker (it's blocked on StreamEvent, not QueueEvent) but we
    // still need to trigger it so that once the stream finishes and
    // the worker loops back to the top of Run(), the subsequent
    // QueueEvent->Wait returns immediately and the loop exits via
    // bStopRequested.
    if (QueueEvent)
    {
        QueueEvent->Trigger();
    }

    // Wait for Run() to return. This is the point where the worker
    // thread actually finishes; any in-flight ProcessMessage call will
    // complete (because we just asked LiteRT-LM to cancel it) before
    // the thread exits.
    if (Thread.IsValid())
    {
        Thread->WaitForCompletion();
        Thread.Reset();
    }

    // Return the events to the pool. Must happen AFTER the thread has
    // joined and AFTER LiteRT-LM's stream callback thread has delivered
    // its final chunk — if we returned either event while another
    // thread might still Trigger/Wait on it, we would access a freed
    // event. Thread->WaitForCompletion guarantees both conditions:
    // the worker thread is done, and the worker only returns from
    // ProcessMessage after StreamEvent has been signaled (which only
    // happens after LiteRT-LM's last callback).
    if (QueueEvent)
    {
        FPlatformProcess::ReturnSynchEventToPool(QueueEvent);
        QueueEvent = nullptr;
    }
    if (StreamEvent)
    {
        FPlatformProcess::ReturnSynchEventToPool(StreamEvent);
        StreamEvent = nullptr;
    }

    // Now it is safe to destroy the native resources. The worker is
    // guaranteed not to be in the middle of any LiteRT-LM call because
    // the thread has finished and the stream callback has delivered
    // its final chunk.
    if (NativeConversation != nullptr)
    {
        litert_lm_conversation_delete(NativeConversation);
        NativeConversation = nullptr;
    }
    if (NativeConversationConfig != nullptr)
    {
        litert_lm_conversation_config_delete(NativeConversationConfig);
        NativeConversationConfig = nullptr;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("FLiteRtLmConversationWorker: destroyed"));
}

void FLiteRtLmConversationWorker::EnqueueMessage(FString UserText)
{
    MessageQueue.Enqueue(MoveTemp(UserText));
    if (QueueEvent)
    {
        QueueEvent->Trigger();
    }
}

void FLiteRtLmConversationWorker::Cancel()
{
    // No-op if nothing is in flight. Reading the atomic once is fine
    // here — the worst case is a TOCTOU where the stream finishes
    // between our read and the cancel_process call, in which case
    // cancel_process is itself harmless (LiteRT-LM handles "cancel
    // with nothing to cancel" gracefully per the C API docs).
    if (!bStreamInFlight.Load())
    {
        return;
    }

    // Mark the stream as cancelled. The worker thread reads this after
    // StreamEvent unblocks and dispatches OnError("Cancelled by caller")
    // instead of OnComplete. The static callback also reads it to
    // suppress further OnToken broadcasts after cancel is requested —
    // if any chunks arrive between the cancel call and LiteRT-LM
    // processing it, they won't leak into the game thread.
    bStreamCancelled = true;

    if (NativeConversation != nullptr)
    {
        litert_lm_conversation_cancel_process(NativeConversation);
    }
}

uint32 FLiteRtLmConversationWorker::Run()
{
    while (!bStopRequested)
    {
        FString UserText;
        if (MessageQueue.Dequeue(UserText))
        {
            ProcessMessage(UserText);
        }
        else
        {
            // Queue is empty — block until either a new message is
            // enqueued or Stop() / destructor triggers the event.
            if (QueueEvent)
            {
                QueueEvent->Wait();
            }
        }
    }
    return 0;
}

void FLiteRtLmConversationWorker::Stop()
{
    bStopRequested = true;
    if (QueueEvent)
    {
        QueueEvent->Trigger();
    }
}

void FLiteRtLmConversationWorker::ProcessMessage(const FString& UserText)
{
    if (NativeConversation == nullptr)
    {
        DispatchErrorOnGameThread(TEXT("Native conversation is null"));
        return;
    }

    // Reset per-send state BEFORE calling into LiteRT-LM. StreamEvent
    // is manual-reset, so we must clear any leftover signal from the
    // previous send. bStreamCancelled is cleared here so that a cancel
    // flag left over from a previous send doesn't poison a new one —
    // the only way a new send starts in the cancelled state is if the
    // destructor has set it AND bStopRequested, in which case we'll
    // short-circuit via the bStopRequested check in Run() anyway.
    StreamAccumulated.Reset();
    StreamError.Reset();
    bStreamCancelled = false;
    if (StreamEvent)
    {
        StreamEvent->Reset();
    }

    // Build the user message JSON. Same shape as the Phase 1
    // ConversationTest smoke test: role=user, content is an array with
    // one text part. EscapeJsonString includes the surrounding quotes
    // (UE convention) so the format string must NOT re-wrap %s in "".
    const FString EscapedPromptQuoted = EscapeJsonString(UserText);
    const FString MessageJson = FString::Printf(
        TEXT(R"({"role":"user","content":[{"type":"text","text":%s}]})"),
        *EscapedPromptQuoted);
    const FTCHARToUTF8 MessageJsonUtf8(*MessageJson);

    // Kick off the non-blocking stream. LiteRT-LM returns immediately;
    // the callback fires on its internal thread for each chunk.
    //
    // We set bStreamInFlight BEFORE the call so that if Cancel races
    // in between the flag set and send_message_stream, the worst case
    // is that Cancel calls cancel_process on a conversation that
    // hasn't actually started streaming yet — the C API handles that
    // gracefully.
    bStreamInFlight = true;

    const int StartRc = litert_lm_conversation_send_message_stream(
        NativeConversation,
        MessageJsonUtf8.Get(),
        /*extra_context=*/ nullptr,
        &FLiteRtLmConversationWorker::OnStreamChunkStatic,
        /*callback_data=*/ this);

    if (StartRc != 0)
    {
        bStreamInFlight = false;
        DispatchErrorOnGameThread(FString::Printf(
            TEXT("litert_lm_conversation_send_message_stream returned non-zero (%d) — stream did not start"),
            StartRc));
        return;
    }

    // Block until the stream's final callback signals StreamEvent.
    // This is the whole reason our worker thread exists: we need a
    // thread that can block on the stream without stalling the game
    // thread. The callback runs on LiteRT-LM's internal thread, not
    // ours, so this Wait doesn't deadlock.
    if (StreamEvent)
    {
        StreamEvent->Wait();
    }

    // Stream is now complete (one way or another). LiteRT-LM will not
    // call OnStreamChunk again for this send — safe to read the
    // accumulated state from the worker thread.
    bStreamInFlight = false;

    // Decide which terminal broadcast to dispatch. Priority: cancel
    // > error > empty text > success.
    if (bStreamCancelled.Load())
    {
        DispatchErrorOnGameThread(TEXT("Cancelled by caller"));
        return;
    }

    if (!StreamError.IsEmpty())
    {
        DispatchErrorOnGameThread(StreamError);
        return;
    }

    if (StreamAccumulated.IsEmpty())
    {
        // Empty text on the happy path is almost always a template /
        // tool-call edge case. Match D.2 semantics: treat as error
        // rather than broadcasting OnComplete("").
        DispatchErrorOnGameThread(
            TEXT("Stream finished with no text content"));
        return;
    }

    DispatchCompleteOnGameThread(StreamAccumulated);
}

void FLiteRtLmConversationWorker::OnStreamChunkStatic(
    void* callback_data,
    const char* chunk,
    bool is_final,
    const char* error_msg)
{
    // Static trampoline. callback_data is the worker instance pointer
    // we passed to send_message_stream. It is guaranteed valid for
    // the lifetime of the stream because the worker thread is blocked
    // inside ProcessMessage's StreamEvent->Wait and ~FLiteRtLmConversationWorker
    // always waits for the final callback before destroying.
    if (callback_data == nullptr)
    {
        return;
    }
    auto* const Self = static_cast<FLiteRtLmConversationWorker*>(callback_data);
    Self->OnStreamChunk(chunk, is_final, error_msg);
}

void FLiteRtLmConversationWorker::OnStreamChunk(
    const char* chunk, bool is_final, const char* error_msg)
{
    // Runs on LiteRT-LM's internal thread. MUST NOT touch UObject
    // state directly. Safe to touch worker instance fields that are
    // synchronized via StreamEvent — StreamAccumulated and StreamError
    // are read only by the worker thread AFTER StreamEvent is signaled
    // (happens-before edge), so writes here are safe as long as we
    // don't signal until we're done writing.

    const bool bCancelledNow = bStreamCancelled.Load();

    // Capture the error message if LiteRT-LM reported one.
    if (error_msg != nullptr && *error_msg != '\0')
    {
        StreamError = FString(UTF8_TO_TCHAR(error_msg));
    }

    // Parse the chunk JSON and extract text parts, then accumulate +
    // dispatch. LiteRT-LM's conversation_send_message_stream delivers
    // each chunk as a FULL assistant message JSON wrapping one token
    // delta, not plain text:
    //     {"role":"assistant","content":[{"type":"text","text":"delta"}, ...]}
    // We unwrap the JSON here so that OnToken callers see only the
    // assistant text delta. Tool-call parts (D.4) will be handled in
    // a separate branch here and routed through OnToolCalled.
    //
    // Suppressed entirely if the stream is cancelled — the final
    // terminal broadcast will be OnError("Cancelled by caller") and
    // leaking tokens through after cancel would confuse observers.
    if (chunk != nullptr && *chunk != '\0' && !bCancelledNow)
    {
        const FString ChunkJson(UTF8_TO_TCHAR(chunk));

        TSharedPtr<FJsonObject> RootObj;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ChunkJson);
        if (FJsonSerializer::Deserialize(Reader, RootObj) && RootObj.IsValid())
        {
            FString ChunkText;
            const TArray<TSharedPtr<FJsonValue>>* ContentArrayPtr = nullptr;
            if (RootObj->TryGetArrayField(TEXT("content"), ContentArrayPtr)
                && ContentArrayPtr != nullptr)
            {
                for (const TSharedPtr<FJsonValue>& PartValue : *ContentArrayPtr)
                {
                    if (!PartValue.IsValid() || PartValue->Type != EJson::Object)
                    {
                        continue;
                    }
                    const TSharedPtr<FJsonObject>& PartObj = PartValue->AsObject();

                    FString PartType;
                    PartObj->TryGetStringField(TEXT("type"), PartType);
                    if (PartType != TEXT("text"))
                    {
                        // D.4: handle type=="tool_call" here.
                        continue;
                    }

                    FString PartText;
                    if (PartObj->TryGetStringField(TEXT("text"), PartText))
                    {
                        ChunkText += PartText;
                    }
                }
            }

            if (!ChunkText.IsEmpty())
            {
                StreamAccumulated += ChunkText;
                DispatchTokenOnGameThread(ChunkText);
            }
        }
        else
        {
            // Parse failure on a chunk is unexpected — LiteRT-LM
            // should always emit well-formed JSON. Log a warning and
            // keep going; accumulated text will just be incomplete
            // for this send. Don't abort the stream — we still want
            // the final callback to fire so the worker unblocks.
            UE_LOG(LogInoAgents, Warning,
                   TEXT("FLiteRtLmConversationWorker: failed to parse stream chunk JSON, dropping: %s"),
                   *ChunkJson);
        }
    }

    // On the final callback (or on error), signal the worker thread
    // to wake up from StreamEvent->Wait and dispatch the terminal
    // OnComplete / OnError. is_final is what LiteRT-LM uses to mark
    // the end of a clean stream; error_msg != nullptr is end-of-stream
    // via failure. Either way, there will be no more callbacks.
    const bool bErrored = (error_msg != nullptr && *error_msg != '\0');
    if (is_final || bErrored)
    {
        if (StreamEvent)
        {
            StreamEvent->Trigger();
        }
    }
}

void FLiteRtLmConversationWorker::DispatchTokenOnGameThread(FString Chunk)
{
    // Called from the stream callback on LiteRT-LM's internal thread
    // (not the worker thread and definitely not the game thread).
    // Capture the weak pointer and chunk by value into the lambda and
    // queue it on the game thread via AsyncTask. The game thread
    // processes AsyncTasks in FIFO order, so tokens dispatched in
    // sequence from the callback arrive on the game thread in the
    // same order — and any dispatches we queue for OnComplete / OnError
    // after the stream finishes are ordered strictly AFTER every
    // OnToken dispatch from the same send.
    TWeakObjectPtr<ULiteRtLmConversation> WeakOwnerCopy = WeakOwner;
    AsyncTask(ENamedThreads::GameThread,
        [WeakOwnerCopy, Chunk = MoveTemp(Chunk)]()
    {
        if (ULiteRtLmConversation* Conv = WeakOwnerCopy.Get())
        {
            Conv->OnToken.Broadcast(Chunk);
        }
    });
}

void FLiteRtLmConversationWorker::DispatchCompleteOnGameThread(FString FullText)
{
    // Capture WeakOwner by value so the lambda has its own copy. The
    // lambda runs on the game thread; the game-thread validity check
    // guards against the UObject having been GC'd between the time
    // the worker dispatches and the time the lambda runs.
    TWeakObjectPtr<ULiteRtLmConversation> WeakOwnerCopy = WeakOwner;
    AsyncTask(ENamedThreads::GameThread,
        [WeakOwnerCopy, FullText = MoveTemp(FullText)]()
    {
        if (ULiteRtLmConversation* Conv = WeakOwnerCopy.Get())
        {
            Conv->OnComplete.Broadcast(FullText);
        }
    });
}

void FLiteRtLmConversationWorker::DispatchErrorOnGameThread(FString ErrorMessage)
{
    TWeakObjectPtr<ULiteRtLmConversation> WeakOwnerCopy = WeakOwner;
    AsyncTask(ENamedThreads::GameThread,
        [WeakOwnerCopy, ErrorMessage = MoveTemp(ErrorMessage)]()
    {
        if (ULiteRtLmConversation* Conv = WeakOwnerCopy.Get())
        {
            Conv->OnError.Broadcast(ErrorMessage);
        }
    });
}
