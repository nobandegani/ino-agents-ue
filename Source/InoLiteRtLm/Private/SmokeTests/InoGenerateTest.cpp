// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.GenerateTest
// ============================================================================
//
// Phase-1 smoke test: loads the engine, creates a session, feeds a text prompt
// through the blocking litert_lm_session_generate_content() API, reads the
// response, and cleans up.
//
// Uses the RAW generation path — no chat template, no tools, no streaming.
// That means an instruction-style prompt like "What is 2+2?" will be treated
// as a text completion and the model will continue the text rather than
// answer. For instruction following, see Ino.ConversationTest.
//
// Runs synchronously on the game thread. Expect ~5-10 seconds of editor
// freeze (engine load + blocking generation).
//
// Invoke:
//     Ino.GenerateTest
//     Ino.GenerateTest Once upon a time, there was
// ============================================================================

#include "InoAgentsLog.h"
#include "InoSmokeTestCommon.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include "litert/lm/engine.h"

static void RunGenerateSmokeTest(const TArray<FString>& Args)
{
    // Default: a classic text-completion prompt. Gemma 4 E2B IT will continue
    // this with a plausible name + short sentence, because
    // litert_lm_session_generate_content does raw next-token generation —
    // it does NOT apply a chat template.
    const FString Prompt = (Args.Num() > 0)
        ? FString::Join(Args, TEXT(" "))
        : FString(TEXT("Hello, my name is"));

    const FString ModelPath = InoSmokeTest::ResolveDefaultModelPath();
    if (ModelPath.IsEmpty())
    {
        return;
    }

    UE_LOG(LogInoAgents, Log, TEXT("GenerateTest: starting"));
    UE_LOG(LogInoAgents, Log, TEXT("  Prompt: \"%s\""), *Prompt);
    UE_LOG(LogInoAgents, Log, TEXT("  Model:  %s"), *ModelPath);
    UE_LOG(LogInoAgents, Warning,
           TEXT("GenerateTest: the editor will freeze for several seconds (engine load + generation)."));
    GLog->Flush();

    // --- UTF-8 buffers (must stay alive through the entire call chain) ---
    const FTCHARToUTF8 ModelPathUtf8(*ModelPath);
    const FTCHARToUTF8 PromptUtf8(*Prompt);

    // --- Load engine ---
    const double T0 = FPlatformTime::Seconds();

    LiteRtLmEngineSettings* Settings = litert_lm_engine_settings_create(
        ModelPathUtf8.Get(), "cpu", nullptr, nullptr);
    if (Settings == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("GenerateTest: engine_settings_create returned NULL"));
        return;
    }

    LiteRtLmEngine* Engine = litert_lm_engine_create(Settings);
    const double TEngineLoaded = FPlatformTime::Seconds();
    if (Engine == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("GenerateTest: engine_create returned NULL after %.2f s"),
               TEngineLoaded - T0);
        litert_lm_engine_settings_delete(Settings);
        return;
    }
    UE_LOG(LogInoAgents, Log, TEXT("GenerateTest: engine loaded in %.2f s"), TEngineLoaded - T0);

    // --- Create a session (default config) ---
    LiteRtLmSession* Session = litert_lm_engine_create_session(Engine, /*config=*/ nullptr);
    const double TSessionCreated = FPlatformTime::Seconds();
    if (Session == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("GenerateTest: engine_create_session returned NULL"));
        litert_lm_engine_delete(Engine);
        litert_lm_engine_settings_delete(Settings);
        return;
    }
    UE_LOG(LogInoAgents, Log,
           TEXT("GenerateTest: session created in %.3f s"),
           TSessionCreated - TEngineLoaded);

    // --- Build LiteRtLmInputData for the prompt ---
    // .data must remain valid until generate_content returns; PromptUtf8 is
    // in an enclosing scope so its buffer outlives the call.
    LiteRtLmInputData TextInput;
    TextInput.type = kLiteRtLmInputDataTypeText;
    TextInput.data = PromptUtf8.Get();
    TextInput.size = static_cast<size_t>(PromptUtf8.Length());

    // --- Generate (blocking) ---
    UE_LOG(LogInoAgents, Log, TEXT("GenerateTest: generating... (this is the slow step)"));
    GLog->Flush();

    const double TGenStart = FPlatformTime::Seconds();
    LiteRtLmResponses* Responses = litert_lm_session_generate_content(
        Session, &TextInput, /*num_inputs=*/ 1);
    const double TGenEnd = FPlatformTime::Seconds();

    if (Responses == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("GenerateTest: session_generate_content returned NULL after %.2f s"),
               TGenEnd - TGenStart);
        litert_lm_session_delete(Session);
        litert_lm_engine_delete(Engine);
        litert_lm_engine_settings_delete(Settings);
        return;
    }

    // --- Read response text ---
    const int NumCandidates = litert_lm_responses_get_num_candidates(Responses);
    UE_LOG(LogInoAgents, Log,
           TEXT("GenerateTest: generation complete in %.2f s, %d candidate(s)"),
           TGenEnd - TGenStart, NumCandidates);

    for (int i = 0; i < NumCandidates; ++i)
    {
        const char* const ResponseTextCStr =
            litert_lm_responses_get_response_text_at(Responses, i);
        if (ResponseTextCStr == nullptr)
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("GenerateTest:   candidate[%d] = <NULL>"), i);
            continue;
        }

        // The returned pointer is owned by Responses and valid only until
        // litert_lm_responses_delete(). Copy into an FString immediately.
        const FString ResponseText = UTF8_TO_TCHAR(ResponseTextCStr);
        UE_LOG(LogInoAgents, Log, TEXT("GenerateTest:   candidate[%d] = \"%s\""),
               i, *ResponseText);
    }

    // --- Cleanup (reverse order of creation) ---
    litert_lm_responses_delete(Responses);
    litert_lm_session_delete(Session);
    litert_lm_engine_delete(Engine);
    litert_lm_engine_settings_delete(Settings);

    const double TEnd = FPlatformTime::Seconds();
    UE_LOG(LogInoAgents, Log,
           TEXT("GenerateTest: DONE — total elapsed %.2f s"),
           TEnd - T0);
}

static FAutoConsoleCommand GGenerateTestCommand(
    TEXT("Ino.GenerateTest"),
    TEXT("Phase-1 smoke test: load engine, create a session, generate a "
         "completion for a text prompt, and clean up. Synchronous on the "
         "game thread; freezes the editor for ~5-10 seconds. Optionally "
         "takes a custom prompt as arguments: "
         "'Ino.GenerateTest Once upon a time'. Uses the raw "
         "generate_content API (no chat template) so instruction prompts "
         "get continued rather than answered — use the conversation API "
         "in a future milestone for instruction following."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunGenerateSmokeTest));
