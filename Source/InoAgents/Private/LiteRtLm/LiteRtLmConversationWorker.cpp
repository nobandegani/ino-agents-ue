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
    // Manual-reset=false: the event auto-resets after Wait returns, so
    // each Trigger wakes the worker exactly once. That matches the
    // "one enqueue = one wake, one Stop = one wake" semantics we want.
    QueueEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/ false);

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
    if (QueueEvent)
    {
        QueueEvent->Trigger();
    }

    // Wait for Run() to return. This is the point where the worker
    // thread actually finishes; any in-flight ProcessMessage call will
    // complete before the thread exits.
    if (Thread.IsValid())
    {
        Thread->WaitForCompletion();
        Thread.Reset();
    }

    // Return the event to the pool. Must happen AFTER the thread has
    // joined — if we returned it while the worker might still call
    // Wait(), we would access a freed event.
    if (QueueEvent)
    {
        FPlatformProcess::ReturnSynchEventToPool(QueueEvent);
        QueueEvent = nullptr;
    }

    // Now it is safe to destroy the native resources. The worker is
    // guaranteed not to be in the middle of any LiteRT-LM call because
    // the thread has finished.
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

    // Build the user message JSON. Same shape as the Phase 1
    // ConversationTest smoke test: role=user, content is an array with
    // one text part. EscapeJsonString includes the surrounding quotes
    // (UE convention) so the format string must NOT re-wrap %s in "".
    const FString EscapedPromptQuoted = EscapeJsonString(UserText);
    const FString MessageJson = FString::Printf(
        TEXT(R"({"role":"user","content":[{"type":"text","text":%s}]})"),
        *EscapedPromptQuoted);
    const FTCHARToUTF8 MessageJsonUtf8(*MessageJson);

    // Blocking send. This is the expensive call — it runs for the
    // entire duration of model inference (hundreds of ms to several
    // seconds). We are on the worker thread, so blocking is fine.
    LiteRtLmJsonResponse* Response = litert_lm_conversation_send_message(
        NativeConversation,
        MessageJsonUtf8.Get(),
        /*extra_context=*/ nullptr);

    if (Response == nullptr)
    {
        DispatchErrorOnGameThread(
            TEXT("litert_lm_conversation_send_message returned NULL"));
        return;
    }

    // Copy the response JSON out of the native response object before
    // deleting it. The native string is only valid while Response is
    // alive.
    const char* const ResponseCStr = litert_lm_json_response_get_string(Response);
    const FString ResponseJson = (ResponseCStr != nullptr)
        ? FString(UTF8_TO_TCHAR(ResponseCStr))
        : FString();
    litert_lm_json_response_delete(Response);

    if (ResponseJson.IsEmpty())
    {
        DispatchErrorOnGameThread(TEXT("Response JSON from LiteRT-LM was empty"));
        return;
    }

    // Parse the response JSON and extract the concatenated assistant
    // text. Response shape (from LiteRT-LM conversation API):
    //     {"role":"assistant","content":[{"type":"text","text":"..."}, ...]}
    TSharedPtr<FJsonObject> RootObj;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseJson);
    if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
    {
        DispatchErrorOnGameThread(FString::Printf(
            TEXT("Failed to parse assistant response JSON: %s"),
            *ResponseJson));
        return;
    }

    FString AssistantText;
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
                continue;
            }

            FString PartText;
            if (PartObj->TryGetStringField(TEXT("text"), PartText))
            {
                AssistantText += PartText;
            }
        }
    }

    // If the response had no text content at all, report it as an error
    // rather than firing OnComplete with an empty string — empty text
    // almost always indicates a template/tool-call edge case and is not
    // what a caller expects on a plain chat send.
    if (AssistantText.IsEmpty())
    {
        DispatchErrorOnGameThread(FString::Printf(
            TEXT("Assistant response had no text content. Raw JSON: %s"),
            *ResponseJson));
        return;
    }

    DispatchCompleteOnGameThread(MoveTemp(AssistantText));
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
