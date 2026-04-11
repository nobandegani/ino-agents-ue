// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmConversation.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmModelConfig.h"
#include "LiteRtLm/LiteRtLmSubsystem.h"
#include "LiteRtLmConversationWorker.h"

#include "Serialization/JsonWriter.h"  // EscapeJsonString

#include "litert/lm/engine.h"

void ULiteRtLmConversation::Initialize(
    ULiteRtLmSubsystem* InSubsystem,
    LiteRtLmEngine* InEngine,
    const ULiteRtLmModelConfig* InConfig)
{
    check(IsInGameThread());

    if (InEngine == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ULiteRtLmConversation::Initialize: engine is null"));
        return;
    }
    if (InConfig == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ULiteRtLmConversation::Initialize: config is null"));
        return;
    }

    Subsystem = InSubsystem;

    // Build the system message JSON, if the config provides one.
    // Format (from LiteRT-LM's C API comments + Phase 1 ConversationTest):
    //     {"type":"text","text":"<escaped>"}
    // EscapeJsonString includes surrounding quotes, so the format string
    // does NOT re-wrap the %s in "".
    FString SystemMessageJson;
    if (!InConfig->SystemMessage.IsEmpty())
    {
        const FString EscapedQuoted = EscapeJsonString(InConfig->SystemMessage);
        SystemMessageJson = FString::Printf(
            TEXT(R"({"type":"text","text":%s})"),
            *EscapedQuoted);
    }

    // Pass a null pointer to the C API when no system message was
    // configured — LiteRT-LM interprets that as "use model default".
    // The UTF-8 converter must outlive the call, so keep it in scope.
    const FTCHARToUTF8 SystemMessageJsonUtf8(*SystemMessageJson);
    const char* const SystemMessageJsonCStr =
        SystemMessageJson.IsEmpty() ? nullptr : SystemMessageJsonUtf8.Get();

    // Create the native conversation config. D.2 leaves tools_json and
    // messages_json null and constrained decoding off — those are the
    // tool-calling bits that D.4 will wire up.
    LiteRtLmConversationConfig* NativeConvConfig = litert_lm_conversation_config_create(
        InEngine,
        /*session_config=*/              nullptr,
        /*system_message_json=*/         SystemMessageJsonCStr,
        /*tools_json=*/                  nullptr,
        /*messages_json=*/               nullptr,
        /*enable_constrained_decoding=*/ false);

    if (NativeConvConfig == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ULiteRtLmConversation::Initialize: "
                    "litert_lm_conversation_config_create returned NULL"));
        return;
    }

    // Create the native conversation. On failure, clean up the config we
    // just built so we don't leak it.
    LiteRtLmConversation* NativeConv = litert_lm_conversation_create(InEngine, NativeConvConfig);
    if (NativeConv == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ULiteRtLmConversation::Initialize: "
                    "litert_lm_conversation_create returned NULL"));
        litert_lm_conversation_config_delete(NativeConvConfig);
        return;
    }

    // Hand the native resources to a new worker. From this point on, the
    // worker owns them — it will destroy them in its destructor when
    // BeginDestroy → Worker.Reset() runs.
    Worker = MakeUnique<FLiteRtLmConversationWorker>(
        TWeakObjectPtr<ULiteRtLmConversation>(this),
        NativeConv,
        NativeConvConfig);

    UE_LOG(LogInoAgents, Log,
           TEXT("ULiteRtLmConversation: initialized (system_message=%s)"),
           InConfig->SystemMessage.IsEmpty() ? TEXT("<none>") : TEXT("<set>"));
}

void ULiteRtLmConversation::SendMessageAsync(const FString& UserText)
{
    check(IsInGameThread());

    if (!Worker.IsValid())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ULiteRtLmConversation::SendMessageAsync: worker is null — "
                    "conversation was not initialized"));
        OnError.Broadcast(TEXT("Conversation not initialized"));
        return;
    }

    Worker->EnqueueMessage(UserText);
}

void ULiteRtLmConversation::BeginDestroy()
{
    // Resetting the TUniquePtr invokes ~FLiteRtLmConversationWorker,
    // which:
    //   1. Signals the worker thread to stop
    //   2. Waits for the worker thread to finish its current message
    //   3. Destroys the native LiteRtLmConversation and ConversationConfig
    //   4. Returns the queue event to the UE pool
    //
    // This is the whole cleanup path for the conversation. No explicit
    // delete of native pointers needed here — the worker owns them.
    Worker.Reset();

    Super::BeginDestroy();
}
