// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.ConversationTest (milestone A)
// ============================================================================
//
// Phase-1 smoke test for the conversation API with chat template. Unlike
// Ino.GenerateTest (raw next-token continuation), this wraps the user
// prompt in the model's instruction-following format via
// litert_lm_conversation_* APIs, so an instruction like "What is 2 plus 2?"
// actually gets answered instead of continued as text.
//
// Still synchronous, no streaming, no tools. Tool calling is
// Ino.ToolCallTest (milestone B).
//
// Invoke:
//     Ino.ConversationTest
//     Ino.ConversationTest Write a haiku about Unreal Engine
// ============================================================================

#include "InoAgentsLog.h"
#include "InoSmokeTestCommon.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonWriter.h"  // EscapeJsonString (quotes included!)

#include "litert/lm/engine.h"

static void RunConversationSmokeTest(const TArray<FString>& Args)
{
    const FString Prompt = (Args.Num() > 0)
        ? FString::Join(Args, TEXT(" "))
        : FString(TEXT("What is 2 plus 2? Answer in one sentence."));

    const FString ModelPath = InoSmokeTest::ResolveDefaultModelPath();
    if (ModelPath.IsEmpty())
    {
        return;
    }

    UE_LOG(LogInoAgents, Log, TEXT("ConversationTest: starting"));
    UE_LOG(LogInoAgents, Log, TEXT("  Prompt: \"%s\""), *Prompt);
    UE_LOG(LogInoAgents, Warning,
           TEXT("ConversationTest: the editor will freeze for several seconds."));
    GLog->Flush();

    // --- UTF-8 buffers (must stay alive through the entire call chain) ---
    const FTCHARToUTF8 ModelPathUtf8(*ModelPath);

    // Build the message JSON. The user prompt is embedded as the text of
    // a single content part.
    //
    // IMPORTANT: UE's EscapeJsonString (Serialization/JsonWriter.h) returns
    // the escaped string WITH surrounding double quotes already appended —
    // i.e. it returns `"Hello"`, not `Hello`. So the format string must NOT
    // wrap %s in its own quotes, or we end up with `""Hello""` and
    // nlohmann::json::parse silently discards the JSON, producing a fast
    // NULL return from litert_lm_conversation_send_message.
    const FString EscapedPromptQuoted = EscapeJsonString(Prompt);
    const FString MessageJson = FString::Printf(
        TEXT(R"({"role":"user","content":[{"type":"text","text":%s}]})"),
        *EscapedPromptQuoted);
    const FTCHARToUTF8 MessageJsonUtf8(*MessageJson);

    UE_LOG(LogInoAgents, Verbose, TEXT("  message_json: %s"), *MessageJson);

    // Pass system message as PLAIN TEXT. The C API's JSON parse will
    // fail on a raw string, so it falls back to using it as-is in
    // {"role":"system","content":"<raw_string>"}. Gemma 4's template
    // expects content to be a string — JSON objects are silently dropped.
    const char* const SystemMessageJsonCStr =
        "You are a helpful assistant. Answer concisely.";

    // --- Load engine ---
    const double T0 = FPlatformTime::Seconds();

    LiteRtLmEngineSettings* Settings = litert_lm_engine_settings_create(
        ModelPathUtf8.Get(), "cpu", nullptr, nullptr);
    if (Settings == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("ConversationTest: engine_settings_create returned NULL"));
        return;
    }

    LiteRtLmEngine* Engine = litert_lm_engine_create(Settings);
    const double TEngineLoaded = FPlatformTime::Seconds();
    if (Engine == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationTest: engine_create returned NULL after %.2f s"),
               TEngineLoaded - T0);
        litert_lm_engine_settings_delete(Settings);
        return;
    }
    UE_LOG(LogInoAgents, Log, TEXT("ConversationTest: engine loaded in %.2f s"), TEngineLoaded - T0);

    // --- Create conversation config ---
    // session_config          = (skipped — default)
    // system_message          = "You are a helpful assistant..."
    // tools_json              = (skipped — no tools in this test, see ToolCallTest)
    // messages_json           = (skipped — no prior history)
    // enable_constrained_decoding = false (only needed with tools)
    //
    // The C API split the old multi-arg create() into a no-arg create()
    // + setter functions in LiteRT-LM SHA 4dbbf937. Only the system
    // message setter is called here.
    LiteRtLmConversationConfig* ConvConfig = litert_lm_conversation_config_create();
    if (ConvConfig == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("ConversationTest: conversation_config_create returned NULL"));
        litert_lm_engine_delete(Engine);
        litert_lm_engine_settings_delete(Settings);
        return;
    }
    if (SystemMessageJsonCStr != nullptr)
    {
        litert_lm_conversation_config_set_system_message(ConvConfig, SystemMessageJsonCStr);
    }
    litert_lm_conversation_config_set_enable_constrained_decoding(ConvConfig, false);

    // --- Create the conversation ---
    LiteRtLmConversation* Conversation = litert_lm_conversation_create(Engine, ConvConfig);
    if (Conversation == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("ConversationTest: conversation_create returned NULL"));
        litert_lm_conversation_config_delete(ConvConfig);
        litert_lm_engine_delete(Engine);
        litert_lm_engine_settings_delete(Settings);
        return;
    }
    UE_LOG(LogInoAgents, Log, TEXT("ConversationTest: conversation created"));

    // --- Send the message (blocking) ---
    UE_LOG(LogInoAgents, Log, TEXT("ConversationTest: sending message..."));
    GLog->Flush();

    const double TSendStart = FPlatformTime::Seconds();
    LiteRtLmJsonResponse* Response = litert_lm_conversation_send_message(
        Conversation, MessageJsonUtf8.Get(), /* extra_context = */ nullptr);
    const double TSendEnd = FPlatformTime::Seconds();

    if (Response == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationTest: conversation_send_message returned NULL after %.2f s"),
               TSendEnd - TSendStart);
        litert_lm_conversation_delete(Conversation);
        litert_lm_conversation_config_delete(ConvConfig);
        litert_lm_engine_delete(Engine);
        litert_lm_engine_settings_delete(Settings);
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationTest: response received in %.2f s"),
           TSendEnd - TSendStart);

    // --- Read response (owned by Response; copy immediately) ---
    const char* const ResponseJsonCStr = litert_lm_json_response_get_string(Response);
    if (ResponseJsonCStr == nullptr)
    {
        UE_LOG(LogInoAgents, Warning, TEXT("ConversationTest: response string is NULL"));
    }
    else
    {
        const FString ResponseJson = UTF8_TO_TCHAR(ResponseJsonCStr);
        UE_LOG(LogInoAgents, Log, TEXT("ConversationTest: response JSON ="));
        UE_LOG(LogInoAgents, Log, TEXT("  %s"), *ResponseJson);
    }

    // --- Cleanup (reverse order of creation, matching upstream tests) ---
    litert_lm_json_response_delete(Response);
    litert_lm_conversation_delete(Conversation);
    litert_lm_conversation_config_delete(ConvConfig);
    litert_lm_engine_delete(Engine);
    litert_lm_engine_settings_delete(Settings);

    const double TEnd = FPlatformTime::Seconds();
    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationTest: DONE — total elapsed %.2f s"),
           TEnd - T0);
}

static FAutoConsoleCommand GConversationTestCommand(
    TEXT("Ino.ConversationTest"),
    TEXT("Phase-1 smoke test (milestone A): load engine, create a conversation "
         "with a system message, send one user message using the chat-template "
         "API, log the response JSON, and clean up. Synchronous. No tools, no "
         "streaming. Optionally takes a custom prompt as arguments: "
         "'Ino.ConversationTest Write a haiku about Unreal Engine'. "
         "Unlike Ino.GenerateTest, this applies the model's chat "
         "template internally — instruction prompts actually get answered."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunConversationSmokeTest));
