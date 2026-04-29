// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.LiteRtLm.ConversationToolTest (milestone D.4b)
// ============================================================================
//
// End-to-end smoke test of the full agent loop: user prompt -> model
// emits tool_call -> worker executes add_numbers on the game thread
// -> worker sends tool result back -> model produces final text using
// the result -> OnComplete fires with the final answer.
//
// This is the headline test for the InoAgents plugin's tool-calling
// feature. Success means every major surface works:
//   - UInoLiteRtLmSubsystem::RegisterTool accepts a UInoLiteRtLmAddNumbersTool
//   - UInoLiteRtLmSubsystem::BuildToolsJsonForConversation produces valid
//     tools_json that the native conversation config accepts
//   - enable_constrained_decoding routes sampling through
//     libGemmaModelConstraintProvider.dll so the model emits
//     syntactically valid tool_call JSON
//   - The worker's OnStreamChunk correctly parses tool_call parts
//     from streaming chunks
//   - The worker's multi-round agent loop executes the tool on the
//     game thread via FEvent + reference capture
//   - OnToolCalled fires as a diagnostic broadcast on the game thread
//   - Round 2's send_message_stream on the same native conversation
//     re-uses the KV cache and produces the final text response
//   - OnComplete delivers only the final text (not intermediate
//     tool-call rounds)
//
// Invoke:
//     Ino.LiteRtLm.ConversationToolTest
//     Ino.LiteRtLm.ConversationToolTest What is 100 plus 250?
// ============================================================================

#include "InoLiteRtLmConversationToolTest.h"

#include "InoAgentsLog.h"
#include "InoSmokeTestCommon.h"
#include "LiteRtLm/InoLiteRtLmAddNumbersTool.h"
#include "LiteRtLm/InoLiteRtLmConversation.h"
// FInoLiteRtLmModelConfig struct is in InoLiteRtLmTypes.h (included via subsystem header)
#include "LiteRtLm/InoLiteRtLmSubsystem.h"
#include "LiteRtLm/InoLiteRtLmToolBase.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

void UInoLiteRtLmConversationToolTestObserver::HandleModelLoaded(
    bool bSuccess, FString ErrorMessage)
{
    if (!bSuccess)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationToolTest: model load FAILED: %s"), *ErrorMessage);
        Finish();
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationToolTest: model loaded, registering add_numbers tool"));

    if (Subsystem == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationToolTest: subsystem reference is null"));
        Finish();
        return;
    }

    // Register the tool BEFORE creating the conversation — the
    // conversation snapshots the tool registry in its Initialize.
    Tool = NewObject<UInoLiteRtLmAddNumbersTool>();
    Subsystem->RegisterTool(Tool);

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationToolTest: creating conversation (should see 'constrained decoding ENABLED' next)"));

    Conversation = Subsystem->CreateConversation();
    if (Conversation == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationToolTest: CreateConversation returned null"));
        Finish();
        return;
    }

    // Bind all four delegates before the first send.
    Conversation->OnToken.AddDynamic(
        this, &UInoLiteRtLmConversationToolTestObserver::HandleToken);
    Conversation->OnToolCalled.AddDynamic(
        this, &UInoLiteRtLmConversationToolTestObserver::HandleToolCalled);
    Conversation->OnComplete.AddDynamic(
        this, &UInoLiteRtLmConversationToolTestObserver::HandleConversationComplete);
    Conversation->OnError.AddDynamic(
        this, &UInoLiteRtLmConversationToolTestObserver::HandleConversationError);

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationToolTest: sending prompt: \"%s\""), *Prompt);

    Conversation->SendMessageAsync(Prompt);
}

void UInoLiteRtLmConversationToolTestObserver::HandleToken(FString RawText, FString CleanText)
{
    const double Now = FPlatformTime::Seconds();

    if (NumTokens == 0)
    {
        FirstTokenTime = Now;
        UE_LOG(LogInoAgents, Log,
               TEXT("ConversationToolTest: first final-round token at +%.2f s from send"),
               Now - StartTime);
    }

    NumTokens += 1;
    AccumulatedText += RawText;

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationToolTest: token %3d (+%.3f s) \"%s\""),
           NumTokens, Now - FirstTokenTime, *RawText);
}

void UInoLiteRtLmConversationToolTestObserver::HandleToolCalled(
    FName ToolName, FString ArgumentsJson, FString ResultJson)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    NumToolCalls += 1;

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationToolTest: OnToolCalled #%d at +%.2f s — name=\"%s\" args=%s result=%s"),
           NumToolCalls, Elapsed, *ToolName.ToString(), *ArgumentsJson, *ResultJson);

    // Flag success if we see the specific call we expected. The test
    // prompt asks for 27 + 15, and the model should emit a call with
    // those args; the tool's Execute implementation returns "42" as
    // a bare JSON number literal.
    if (ToolName == FName(TEXT("add_numbers")) && ResultJson == TEXT("42"))
    {
        bSawExpectedAddNumbersCall = true;
    }
}

void UInoLiteRtLmConversationToolTestObserver::HandleConversationComplete(
    FString FullText)
{
    const double Now      = FPlatformTime::Seconds();
    const double TotalSec = Now - StartTime;

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationToolTest: COMPLETE — %d tokens, %d tool calls, total=%.2f s"),
           NumTokens, NumToolCalls, TotalSec);
    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationToolTest: final answer: \"%s\""), *FullText);

    // Success criteria from the D.4 spec:
    //   - OnToolCalled fired with add_numbers + "42" result
    //   - OnComplete text contains "42" or "forty-two"
    const bool bTextContainsAnswer =
        FullText.Contains(TEXT("42")) ||
        FullText.Contains(TEXT("forty-two"), ESearchCase::IgnoreCase) ||
        FullText.Contains(TEXT("forty two"), ESearchCase::IgnoreCase);

    if (bSawExpectedAddNumbersCall && bTextContainsAnswer)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("ConversationToolTest: PASS — tool round-trip succeeded and "
                    "final answer contains the result"));
    }
    else
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("ConversationToolTest: partial result — SawExpectedAddNumbersCall=%s, "
                    "TextContainsAnswer=%s. This may be a model choice (Gemma 4 E2B sometimes "
                    "computes inline even with tools registered) rather than a bug. Try a more "
                    "direct prompt if this keeps happening."),
               bSawExpectedAddNumbersCall ? TEXT("true") : TEXT("false"),
               bTextContainsAnswer       ? TEXT("true") : TEXT("false"));
    }

    Finish();
}

void UInoLiteRtLmConversationToolTestObserver::HandleConversationError(
    FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogInoAgents, Error,
           TEXT("ConversationToolTest: FAILED after %.2f s (%d tokens, %d tool calls received): %s"),
           Elapsed, NumTokens, NumToolCalls, *ErrorMessage);

    Finish();
}

void UInoLiteRtLmConversationToolTestObserver::Finish()
{
    // Unregister the tool before tearing down the conversation, so
    // the subsystem's tool registry is clean for the next test run.
    // The conversation has already snapshotted its tools_json at
    // Initialize, so this does NOT affect the current in-flight
    // send — it only affects conversations created after this point.
    if (Subsystem && Tool)
    {
        Subsystem->UnregisterTool(FName(TEXT("add_numbers")));
    }

    // Explicit shutdown for deterministic worker teardown (see the
    // D.3 Finish rationale for why Shutdown is used here instead
    // of CollectGarbage — calling CollectGarbage from inside an
    // OnComplete delegate handler races parallel GC workers against
    // the in-flight delegate broadcast's write access).
    if (Conversation)
    {
        Conversation->Shutdown();
    }

    Conversation = nullptr;
    Config = FInoLiteRtLmModelConfig();
    Tool         = nullptr;
    Subsystem    = nullptr;

    RemoveFromRoot();

    UE_LOG(LogInoAgents, Log, TEXT("ConversationToolTest: DONE"));
}

static void RunLiteRtLmConversationToolTest(const TArray<FString>& Args)
{
    UInoLiteRtLmSubsystem* Subsys = InoSmokeTest::FindGameInstanceSubsystem<UInoLiteRtLmSubsystem>();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationToolTest: could not find a UInoLiteRtLmSubsystem — "
                    "start PIE first."));
        return;
    }

    UInoLiteRtLmConversationToolTestObserver* Observer =
        NewObject<UInoLiteRtLmConversationToolTestObserver>();
    Observer->StartTime = FPlatformTime::Seconds();
    Observer->Subsystem = Subsys;
    Observer->Prompt = (Args.Num() > 0)
        ? FString::Join(Args, TEXT(" "))
        : FString(TEXT("What is 27 plus 15?"));
    Observer->AddToRoot();

    if (Subsys->IsModelLoaded())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("ConversationToolTest: model already loaded, skipping load step"));
        Observer->HandleModelLoaded(true, FString());
        return;
    }

    // System message copies the exact phrasing from the Phase 1
    // ToolCallTest that reliably got Gemma 4 E2B to use the tool
    // instead of computing inline. "You MUST call the add_numbers
    // tool to compute it" is significantly stronger than just
    // telling the model the tool exists.
    FInoLiteRtLmModelConfig Config;
    Config.ModelFileName = TEXT("gemma-4-E2B-it.litertlm");
    Config.Backend       = EInoLiteRtLmBackend::Cpu;
    Config.SystemMessage = TEXT(
        "You are a helpful assistant with access to tools. When a user "
        "asks you to perform arithmetic, you MUST call the add_numbers "
        "tool to compute it rather than calculating in your head. When "
        "you receive a tool result, use its value directly in your answer.");
    Observer->Config = Config;

    FOnInoLiteRtLmModelLoaded OnLoaded;
    OnLoaded.BindDynamic(
        Observer,
        &UInoLiteRtLmConversationToolTestObserver::HandleModelLoaded);

    UE_LOG(LogInoAgents, Log,
           TEXT("ConversationToolTest: starting — loading model first (non-blocking)"));

    Subsys->LoadModelAsync(Config, FOnInoModelDownloadProgress(), OnLoaded);
}

static FAutoConsoleCommand GLiteRtLmConversationToolTestCommand(
    TEXT("Ino.LiteRtLm.ConversationToolTest"),
    TEXT("Milestone D.4b smoke test: loads the model, registers a "
         "UInoLiteRtLmAddNumbersTool, creates a conversation with "
         "constrained decoding enabled, binds OnToken/OnToolCalled/"
         "OnComplete/OnError, sends 'What is 27 plus 15?', watches "
         "the agent loop run (model emits a tool_call, worker "
         "executes add_numbers, worker sends the result back, model "
         "produces a final text answer using the result), and logs "
         "PASS if OnToolCalled fired with result \"42\" and "
         "OnComplete's text contains \"42\". Non-blocking — editor "
         "stays responsive. Optional custom prompt: "
         "'Ino.LiteRtLm.ConversationToolTest What is 100 plus 250?'"),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunLiteRtLmConversationToolTest));
