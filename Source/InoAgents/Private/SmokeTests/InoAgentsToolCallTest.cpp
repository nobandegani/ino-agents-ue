// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// InoAgents.ToolCallTest (milestone B)
// ============================================================================
//
// Phase-1 smoke test for the full agent loop. The model sees a prompt,
// decides to call a tool, we execute the tool locally, send the result back,
// and the model produces a final answer using that result. This is the
// headline feature of the InoAgents plugin.
//
// Exposes one in-process tool: add_numbers(a, b). The default prompt is
// "What is 27 plus 15?". On success the model emits a tool call like
// {name:"add_numbers", arguments:{a:27, b:15}}; we compute 42 locally, send
// it back as a tool_response, and the model's final answer uses it.
//
// Why add_numbers and not get_current_time: Gemma 4 E2B is RLHF-trained to
// refuse to report the current time even when handed the data via tool
// result. A pure math function has no such training baggage, so it's a
// cleaner test of whether tool results actually round-trip into the model's
// response.
//
// enable_constrained_decoding = true routes the model's output through
// libGemmaModelConstraintProvider.dll (which is exactly why that DLL is a
// required runtime sibling of LiteRtLm.dll) so that when a tool call is
// expected, the sampler is constrained to emit syntactically valid function
// call JSON.
//
// Still synchronous and still on the game thread. Editor will freeze for
// ~5-15 seconds (engine load + two generation passes).
//
// Invoke:
//     InoAgents.ToolCallTest
//     InoAgents.ToolCallTest What is 100 plus 250?
// ============================================================================

#include "InoAgentsLog.h"
#include "InoAgentsSmokeTestCommon.h"

#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonWriter.h"  // EscapeJsonString (quotes included!)

#include "litert/lm/engine.h"

namespace
{
    /**
     * Local implementation of the one tool this smoke test exposes. Reads
     * 'a' and 'b' from the arguments object (accepting both JSON numbers
     * and numeric strings, because small models emit both) and returns
     * their integer sum on success.
     */
    bool ExecuteAddNumbersTool(const TSharedPtr<FJsonObject>& ArgumentsObj,
                               int64& OutSum,
                               FString& OutErrorMessage)
    {
        if (!ArgumentsObj.IsValid())
        {
            OutErrorMessage = TEXT("arguments object is null");
            return false;
        }

        auto ReadIntField = [&](const TCHAR* FieldName, int64& Out) -> bool
        {
            double AsNumber = 0.0;
            if (ArgumentsObj->TryGetNumberField(FieldName, AsNumber))
            {
                Out = static_cast<int64>(AsNumber);
                return true;
            }
            FString AsString;
            if (ArgumentsObj->TryGetStringField(FieldName, AsString))
            {
                if (AsString.IsNumeric())
                {
                    Out = FCString::Atoi64(*AsString);
                    return true;
                }
            }
            return false;
        };

        int64 A = 0;
        int64 B = 0;
        if (!ReadIntField(TEXT("a"), A))
        {
            OutErrorMessage = TEXT("missing or non-numeric argument 'a'");
            return false;
        }
        if (!ReadIntField(TEXT("b"), B))
        {
            OutErrorMessage = TEXT("missing or non-numeric argument 'b'");
            return false;
        }

        OutSum = A + B;
        return true;
    }
}

static void RunToolCallSmokeTest(const TArray<FString>& Args)
{
    const FString Prompt = (Args.Num() > 0)
        ? FString::Join(Args, TEXT(" "))
        : FString(TEXT("What is 27 plus 15?"));

    const FString ModelPath = InoAgentsSmokeTest::ResolveDefaultModelPath();
    if (ModelPath.IsEmpty())
    {
        return;
    }

    UE_LOG(LogInoAgents, Log, TEXT("ToolCallTest: starting"));
    UE_LOG(LogInoAgents, Log, TEXT("  Prompt: \"%s\""), *Prompt);
    UE_LOG(LogInoAgents, Warning,
           TEXT("ToolCallTest: editor will freeze for ~5-15 seconds (engine + 2 generation passes)."));
    GLog->Flush();

    // --- Static JSON strings (owned here, lifetimes simple) ---
    // A single tool: add_numbers. OpenAI-style function schema with two
    // required integer parameters.
    const char* const ToolsJsonCStr = R"([
        {
            "type": "function",
            "function": {
                "name": "add_numbers",
                "description": "Adds two integers and returns their sum. Always use this tool when the user asks to add, sum, or total two numbers — do not compute in your head.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "a": {"type": "integer", "description": "first integer addend"},
                        "b": {"type": "integer", "description": "second integer addend"}
                    },
                    "required": ["a", "b"]
                }
            }
        }
    ])";

    const char* const SystemMessageJsonCStr =
        R"({"type":"text","text":"You are a helpful assistant with access to tools. When a user asks you to perform arithmetic, you MUST call the add_numbers tool to compute it rather than calculating in your head. When you receive a tool result, use its value directly in your answer."})";

    // --- UTF-8 buffer for the model path ---
    const FTCHARToUTF8 ModelPathUtf8(*ModelPath);

    // --- Load engine ---
    const double T0 = FPlatformTime::Seconds();

    LiteRtLmEngineSettings* Settings = litert_lm_engine_settings_create(
        ModelPathUtf8.Get(), "cpu", nullptr, nullptr);
    if (Settings == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("ToolCallTest: engine_settings_create returned NULL"));
        return;
    }

    LiteRtLmEngine* Engine = litert_lm_engine_create(Settings);
    if (Engine == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("ToolCallTest: engine_create returned NULL"));
        litert_lm_engine_settings_delete(Settings);
        return;
    }
    const double TEngineLoaded = FPlatformTime::Seconds();
    UE_LOG(LogInoAgents, Log, TEXT("ToolCallTest: engine loaded in %.2f s"), TEngineLoaded - T0);

    // --- Create conversation config with tools + constrained decoding ---
    LiteRtLmConversationConfig* ConvConfig = litert_lm_conversation_config_create(
        Engine,
        /* session_config              = */ nullptr,
        /* system_message_json         = */ SystemMessageJsonCStr,
        /* tools_json                  = */ ToolsJsonCStr,
        /* messages_json               = */ nullptr,
        /* enable_constrained_decoding = */ true);
    if (ConvConfig == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("ToolCallTest: conversation_config_create returned NULL"));
        litert_lm_engine_delete(Engine);
        litert_lm_engine_settings_delete(Settings);
        return;
    }

    LiteRtLmConversation* Conversation = litert_lm_conversation_create(Engine, ConvConfig);
    if (Conversation == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("ToolCallTest: conversation_create returned NULL"));
        litert_lm_conversation_config_delete(ConvConfig);
        litert_lm_engine_delete(Engine);
        litert_lm_engine_settings_delete(Settings);
        return;
    }
    UE_LOG(LogInoAgents, Log, TEXT("ToolCallTest: conversation created (with tools, constrained decoding ON)"));

    // --- Build user message JSON ---
    const FString EscapedPromptQuoted = EscapeJsonString(Prompt);
    const FString UserMessageJson = FString::Printf(
        TEXT(R"({"role":"user","content":[{"type":"text","text":%s}]})"),
        *EscapedPromptQuoted);
    const FTCHARToUTF8 UserMessageJsonUtf8(*UserMessageJson);

    // --- Round 1: send user message, get response (text or tool call) ---
    UE_LOG(LogInoAgents, Log, TEXT("ToolCallTest: round 1 — sending user message..."));
    GLog->Flush();

    const double TR1Start = FPlatformTime::Seconds();
    LiteRtLmJsonResponse* Response1 = litert_lm_conversation_send_message(
        Conversation, UserMessageJsonUtf8.Get(), /*extra_context=*/ nullptr);
    const double TR1End = FPlatformTime::Seconds();

    if (Response1 == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ToolCallTest: round 1 send_message returned NULL after %.2f s"),
               TR1End - TR1Start);
        litert_lm_conversation_delete(Conversation);
        litert_lm_conversation_config_delete(ConvConfig);
        litert_lm_engine_delete(Engine);
        litert_lm_engine_settings_delete(Settings);
        return;
    }

    const char* const Response1CStr = litert_lm_json_response_get_string(Response1);
    const FString Response1Json = Response1CStr ? FString(UTF8_TO_TCHAR(Response1CStr)) : FString();

    UE_LOG(LogInoAgents, Log, TEXT("ToolCallTest: round 1 response in %.2f s"), TR1End - TR1Start);
    UE_LOG(LogInoAgents, Log, TEXT("  %s"), *Response1Json);

    const TSharedPtr<FJsonObject> Response1Obj =
        InoAgentsSmokeTest::ParseJsonObjectOrLog(Response1Json, TEXT("ToolCallTest round 1"));
    if (!Response1Obj.IsValid())
    {
        litert_lm_json_response_delete(Response1);
        litert_lm_conversation_delete(Conversation);
        litert_lm_conversation_config_delete(ConvConfig);
        litert_lm_engine_delete(Engine);
        litert_lm_engine_settings_delete(Settings);
        return;
    }

    // --- Check for a tool call ---
    FString ToolName;
    TSharedPtr<FJsonObject> ArgumentsObj;
    const bool bHasToolCall =
        InoAgentsSmokeTest::TryExtractFirstToolCall(Response1Obj, ToolName, ArgumentsObj);

    if (!bHasToolCall)
    {
        const FString AssistantText = InoAgentsSmokeTest::ExtractAssistantText(Response1Obj);
        UE_LOG(LogInoAgents, Warning,
               TEXT("ToolCallTest: model did NOT request a tool call. "
                    "Plain-text reply: \"%s\""),
               *AssistantText);
        UE_LOG(LogInoAgents, Warning,
               TEXT("ToolCallTest: this is not a failure — just means Gemma 4 E2B "
                    "chose not to use the tool for this prompt. Try a more direct "
                    "instruction."));

        litert_lm_json_response_delete(Response1);
        litert_lm_conversation_delete(Conversation);
        litert_lm_conversation_config_delete(ConvConfig);
        litert_lm_engine_delete(Engine);
        litert_lm_engine_settings_delete(Settings);
        return;
    }

    UE_LOG(LogInoAgents, Log, TEXT("ToolCallTest: model called tool \"%s\""), *ToolName);

    // --- Execute the tool locally ---
    // ToolResultValueJsonLiteral is a self-contained JSON literal (either a
    // bare number like "42" or a quoted JSON string like "\"ERROR: ...\"")
    // that will be embedded directly into the tool_response.value field
    // without additional quoting.
    FString ToolResultValueJsonLiteral;
    if (ToolName == TEXT("add_numbers"))
    {
        int64 Sum = 0;
        FString LocalErr;
        if (ExecuteAddNumbersTool(ArgumentsObj, Sum, LocalErr))
        {
            // Emit the sum as a bare JSON number — no quotes, no escaping.
            ToolResultValueJsonLiteral = FString::Printf(TEXT("%lld"), Sum);
            UE_LOG(LogInoAgents, Log,
                   TEXT("ToolCallTest: local tool result: add_numbers -> %lld"), Sum);
        }
        else
        {
            ToolResultValueJsonLiteral = EscapeJsonString(
                FString::Printf(TEXT("ERROR: %s"), *LocalErr));
            UE_LOG(LogInoAgents, Warning,
                   TEXT("ToolCallTest: add_numbers failed: %s"), *LocalErr);
        }
    }
    else
    {
        ToolResultValueJsonLiteral = EscapeJsonString(
            FString::Printf(TEXT("ERROR: unknown tool '%s'"), *ToolName));
        UE_LOG(LogInoAgents, Warning,
               TEXT("ToolCallTest: unexpected tool name '%s'"), *ToolName);
    }

    // --- Build tool result message ---
    // Shape (from runtime/conversation/model_data_processor/gemma4_data_processor_test.cc,
    // MessageToTemplateInputWithToolResponseWithNonObjectValue): tool_response.value
    // may be a scalar — a number or a string — and the data processor renders it
    // as "<tool_name>{value:<ctrl46><scalar><ctrl46>}" in the text the model sees.
    const FString EscapedToolNameQuoted = EscapeJsonString(ToolName);
    const FString ToolResultMessageJson = FString::Printf(
        TEXT(R"({"role":"tool","content":[{"type":"tool_response","tool_response":{"name":%s,"value":%s}}]})"),
        *EscapedToolNameQuoted,
        *ToolResultValueJsonLiteral);
    const FTCHARToUTF8 ToolResultMessageJsonUtf8(*ToolResultMessageJson);

    UE_LOG(LogInoAgents, Log, TEXT("ToolCallTest: tool result message: %s"), *ToolResultMessageJson);

    // --- Round 2: send tool result, get final text answer ---
    UE_LOG(LogInoAgents, Log, TEXT("ToolCallTest: round 2 — sending tool result..."));
    GLog->Flush();

    const double TR2Start = FPlatformTime::Seconds();
    LiteRtLmJsonResponse* Response2 = litert_lm_conversation_send_message(
        Conversation, ToolResultMessageJsonUtf8.Get(), /*extra_context=*/ nullptr);
    const double TR2End = FPlatformTime::Seconds();

    if (Response2 == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ToolCallTest: round 2 send_message returned NULL after %.2f s"),
               TR2End - TR2Start);
        litert_lm_json_response_delete(Response1);
        litert_lm_conversation_delete(Conversation);
        litert_lm_conversation_config_delete(ConvConfig);
        litert_lm_engine_delete(Engine);
        litert_lm_engine_settings_delete(Settings);
        return;
    }

    const char* const Response2CStr = litert_lm_json_response_get_string(Response2);
    const FString Response2Json = Response2CStr ? FString(UTF8_TO_TCHAR(Response2CStr)) : FString();

    UE_LOG(LogInoAgents, Log, TEXT("ToolCallTest: round 2 response in %.2f s"), TR2End - TR2Start);
    UE_LOG(LogInoAgents, Log, TEXT("  %s"), *Response2Json);

    const TSharedPtr<FJsonObject> Response2Obj =
        InoAgentsSmokeTest::ParseJsonObjectOrLog(Response2Json, TEXT("ToolCallTest round 2"));
    if (Response2Obj.IsValid())
    {
        const FString FinalText = InoAgentsSmokeTest::ExtractAssistantText(Response2Obj);
        UE_LOG(LogInoAgents, Log, TEXT("ToolCallTest: final assistant text: \"%s\""), *FinalText);
    }

    // --- Cleanup ---
    litert_lm_json_response_delete(Response2);
    litert_lm_json_response_delete(Response1);
    litert_lm_conversation_delete(Conversation);
    litert_lm_conversation_config_delete(ConvConfig);
    litert_lm_engine_delete(Engine);
    litert_lm_engine_settings_delete(Settings);

    const double TEnd = FPlatformTime::Seconds();
    UE_LOG(LogInoAgents, Log,
           TEXT("ToolCallTest: DONE — total elapsed %.2f s (round 1: %.2f s, round 2: %.2f s)"),
           TEnd - T0, TR1End - TR1Start, TR2End - TR2Start);
}

static FAutoConsoleCommand GToolCallTestCommand(
    TEXT("InoAgents.ToolCallTest"),
    TEXT("Phase-1 smoke test (milestone B): full agent loop. Registers an "
         "'add_numbers(a, b)' tool with the conversation, sends a user prompt "
         "('What is 27 plus 15?' by default), waits for the model to emit a "
         "tool call with parsed integer arguments, executes add locally, "
         "sends the sum back as a tool result, and logs the model's final "
         "answer. Synchronous, freezes the editor ~5-15 seconds. Optionally "
         "takes a custom prompt: 'InoAgents.ToolCallTest What is 100 plus 250?'"),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunToolCallSmokeTest));
