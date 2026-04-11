// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoAgents.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"  // EscapeJsonString() used by ConversationTest

// LiteRT-LM C API. Staged into Source/ThirdParty/InoAgentsLibrary/Public/ by
// LiteRtLm/scripts/build-win64.ps1. The include path is added via
// PublicSystemIncludePaths in InoAgentsLibrary.Build.cs.
#include "litert/lm/engine.h"

DEFINE_LOG_CATEGORY_STATIC(LogInoAgents, Log, All);

namespace
{
    /**
     * Resolve the absolute path to a runtime DLL staged alongside the plugin
     * binaries. Returns an empty string on unsupported platforms.
     */
    FString ResolveStagedDllPath(const TCHAR* DllFileName)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (!Plugin.IsValid())
        {
            return FString();
        }

        const FString BaseDir = Plugin->GetBaseDir();

#if PLATFORM_WINDOWS
        return FPaths::Combine(BaseDir, TEXT("Binaries/ThirdParty/InoAgentsLibrary/Win64"), DllFileName);
#else
        // Phases 2-5 (Android, iOS, Linux, macOS) are not yet implemented.
        // Returning empty causes GetDllHandle() below to no-op gracefully,
        // which is the correct behavior during phase 1.
        return FString();
#endif
    }

    /**
     * Load a staged runtime DLL by filename. Logs on success and failure.
     *
     * Unlike the stock UE "Third Party Library" plugin template, this does
     * NOT show a blocking MessageDialog on failure — that dialog pops up
     * every editor start if a single DLL is missing, which is hostile during
     * development. An error log is sufficient; later layers of the plugin
     * surface the failure to Blueprint via UInoAgentsSubsystem::LoadModel.
     */
    void* LoadStagedDll(const TCHAR* DllFileName)
    {
        const FString Path = ResolveStagedDllPath(DllFileName);
        if (Path.IsEmpty())
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("InoAgents: not loading %s (unsupported platform in phase 1)."),
                   DllFileName);
            return nullptr;
        }

        void* Handle = FPlatformProcess::GetDllHandle(*Path);
        if (Handle)
        {
            UE_LOG(LogInoAgents, Log,
                   TEXT("InoAgents: loaded %s from %s"),
                   DllFileName, *Path);
        }
        else
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("InoAgents: failed to load %s from %s. Did you run "
                        "Plugins/InoAgents/LiteRtLm/scripts/build-win64.ps1?"),
                   DllFileName, *Path);
        }
        return Handle;
    }
}

void FInoAgentsModule::StartupModule()
{
    // LiteRtLm.dll depends on libGemmaModelConstraintProvider.dll at runtime.
    // Load the constraint provider FIRST so it is already resolved in memory
    // when Windows processes LiteRtLm.dll's import table.
    GemmaConstraintProviderHandle = LoadStagedDll(TEXT("libGemmaModelConstraintProvider.dll"));
    LiteRtLmHandle = LoadStagedDll(TEXT("LiteRtLm.dll"));

    // --------------------------------------------------------------
    // Smoke test: call one trivial C API function so we know that
    //   (a) InoAgentsLibrary.Build.cs's import lib is wired correctly,
    //   (b) the /EXPORT: workaround actually produces a callable symbol,
    //   (c) delay-load trampolines resolve on first call without crashing,
    //   (d) UE -> LiteRT-LM calling convention works end-to-end.
    //
    // litert_lm_set_min_log_level is the cheapest function in the public
    // API: no state, no allocation, no model file required. It just forwards
    // to absl::SetMinLogLevel. Arg 0 = INFO (no change in log verbosity).
    //
    // If this crashes, stop here and debug — everything downstream depends
    // on DLL calls working.
    // --------------------------------------------------------------
    if (LiteRtLmHandle)
    {
        litert_lm_set_min_log_level(0);
        UE_LOG(LogInoAgents, Log,
               TEXT("InoAgents: smoke test passed — litert_lm_set_min_log_level(0) returned cleanly."));
    }
}

// ============================================================================
// Phase-1 load-engine smoke test
// ============================================================================
//
// Console command: InoAgents.LoadEngineTest
//
// Purpose: prove that LiteRT-LM can open and parse a real .litertlm model
// through our integration. Calls engine_settings_create -> engine_create ->
// engine_delete -> engine_settings_delete, reports wall-clock duration, and
// exits. Does not create a session, does not generate text — that is the
// next milestone.
//
// This runs synchronously on the game thread and will freeze the editor for
// approximately 5-30 seconds depending on disk speed and CPU. That is the
// whole point of the test: prove the engine can load before we build the
// async / FRunnable machinery that keeps the game thread alive during load.
//
// Invoke from the editor's console (backtick key) or the Output Log's
// command input:
//
//     InoAgents.LoadEngineTest
//
// Model file is resolved from the plugin's own directory at:
//     Plugins/InoAgents/Models/gemma-4-E2B-it.litertlm
//
// Download it from:
//     https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm
// ============================================================================

static void RunLoadEngineSmokeTest(const TArray<FString>& /*Args*/)
{
    // --- 1. Resolve model path via IPluginManager ---
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LoadEngineTest: IPluginManager could not locate the InoAgents plugin. "
                    "This should be impossible — something is very wrong with the module state."));
        return;
    }

    const FString BaseDir = Plugin->GetBaseDir();
    const FString ModelPath = FPaths::Combine(
        BaseDir, TEXT("Models"), TEXT("gemma-4-E2B-it.litertlm"));

    // --- 2. Verify file exists ---
    if (!IFileManager::Get().FileExists(*ModelPath))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LoadEngineTest: model file not found at %s. Download from "
                    "https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm "
                    "and save as Plugins/InoAgents/Models/gemma-4-E2B-it.litertlm"),
               *ModelPath);
        return;
    }

    const int64 FileSizeBytes = IFileManager::Get().FileSize(*ModelPath);
    const double FileSizeGB = static_cast<double>(FileSizeBytes) / (1024.0 * 1024.0 * 1024.0);

    // --- 3. Log what we are about to do ---
    UE_LOG(LogInoAgents, Log, TEXT("LoadEngineTest: starting synchronous engine load"));
    UE_LOG(LogInoAgents, Log, TEXT("  Model path : %s"), *ModelPath);
    UE_LOG(LogInoAgents, Log, TEXT("  Model size : %.2f GB (%lld bytes)"), FileSizeGB, FileSizeBytes);
    UE_LOG(LogInoAgents, Log, TEXT("  Backend    : cpu"));
    UE_LOG(LogInoAgents, Warning,
           TEXT("LoadEngineTest: the editor will freeze for the next 5-30 seconds. "
                "This is expected for phase 1."));

    // Force a log flush so the warning actually appears before the freeze.
    GLog->Flush();

    // --- 4. Build the UTF-8 path for the C API ---
    // IMPORTANT: FTCHARToUTF8's buffer must stay alive for the entire call
    // chain that uses it, so we keep the converter object in scope.
    const FTCHARToUTF8 ModelPathUtf8(*ModelPath);
    const char* const ModelPathCStr = ModelPathUtf8.Get();

    // --- 5. Create engine settings ---
    const double T0 = FPlatformTime::Seconds();

    LiteRtLmEngineSettings* Settings = litert_lm_engine_settings_create(
        /* model_path         = */ ModelPathCStr,
        /* backend_str        = */ "cpu",
        /* vision_backend_str = */ nullptr,
        /* audio_backend_str  = */ nullptr);

    if (Settings == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LoadEngineTest: litert_lm_engine_settings_create returned NULL"));
        return;
    }

    const double T1 = FPlatformTime::Seconds();
    UE_LOG(LogInoAgents, Log,
           TEXT("LoadEngineTest: engine settings created in %.3f s"),
           T1 - T0);

    // --- 6. Create the engine (the slow step — actually parses weights) ---
    LiteRtLmEngine* Engine = litert_lm_engine_create(Settings);
    const double T2 = FPlatformTime::Seconds();

    if (Engine == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LoadEngineTest: litert_lm_engine_create returned NULL after %.2f s. "
                    "Check the LiteRT-LM internal logs above for the underlying reason."),
               T2 - T1);
        litert_lm_engine_settings_delete(Settings);
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("LoadEngineTest: engine loaded successfully in %.2f s (total including settings: %.2f s)"),
           T2 - T1, T2 - T0);

    // --- 7. Clean up ---
    // Destroy the engine first (it holds references into the settings-derived
    // internal state), then the settings object, then let the FTCHARToUTF8
    // converter fall out of scope.
    litert_lm_engine_delete(Engine);
    const double T3 = FPlatformTime::Seconds();

    litert_lm_engine_settings_delete(Settings);
    const double T4 = FPlatformTime::Seconds();

    UE_LOG(LogInoAgents, Log,
           TEXT("LoadEngineTest: engine destroyed in %.3f s, settings destroyed in %.3f s"),
           T3 - T2, T4 - T3);
    UE_LOG(LogInoAgents, Log,
           TEXT("LoadEngineTest: DONE — total elapsed %.2f s"),
           T4 - T0);
}

// Registering via FAutoConsoleCommand makes the command available as soon as
// this translation unit's static initializers run, which is during module
// load. The callback is only invoked when the user actually types the command,
// so DLL symbols are resolved at first-invocation time — by which point
// FInoAgentsModule::StartupModule has already loaded LiteRtLm.dll.
static FAutoConsoleCommand GLoadEngineTestCommand(
    TEXT("InoAgents.LoadEngineTest"),
    TEXT("Phase-1 smoke test: synchronously load and destroy a LiteRT-LM engine "
         "from Plugins/InoAgents/Models/gemma-4-E2B-it.litertlm on the game "
         "thread. Freezes the editor for 5-30s. No session or generation; "
         "this only proves the engine can be constructed."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunLoadEngineSmokeTest));


// ============================================================================
// Phase-1 generate smoke test
// ============================================================================
//
// Console command: InoAgents.GenerateTest [optional prompt words...]
//
// Purpose: prove that the full prompt -> response path works. Loads the
// engine, creates a session, feeds a text prompt through the blocking
// litert_lm_session_generate_content() API, reads the response, and cleans
// up. Does NOT use streaming (no C callback) and does NOT use the
// conversation / chat-template API — this is raw next-token continuation,
// not instruction following. See the comment on the default prompt below.
//
// Still synchronous on the game thread: expect ~5-10 seconds of editor freeze
// (engine load ~2.5s + generation ~2-7s depending on response length).
//
// Invoke from the editor's console:
//     InoAgents.GenerateTest
//     InoAgents.GenerateTest Once upon a time, there was
// ============================================================================

static void RunGenerateSmokeTest(const TArray<FString>& Args)
{
    // --- Optional prompt from console args ---
    //
    // Default: a classic text-completion prompt. Gemma 4 E2B IT should
    // continue this with a plausible name + short sentence, because
    // litert_lm_session_generate_content does raw next-token generation —
    // it does NOT apply a chat template. An instruction-style prompt like
    // "What is 2+2?" will NOT produce an answer here; the model will
    // continue the question. Use the conversation API (future milestone)
    // for instruction following.
    const FString Prompt = (Args.Num() > 0)
        ? FString::Join(Args, TEXT(" "))
        : FString(TEXT("Hello, my name is"));

    // --- Resolve model path (same as LoadEngineTest) ---
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogInoAgents, Error, TEXT("GenerateTest: IPluginManager cannot find the InoAgents plugin."));
        return;
    }
    const FString BaseDir = Plugin->GetBaseDir();
    const FString ModelPath = FPaths::Combine(
        BaseDir, TEXT("Models"), TEXT("gemma-4-E2B-it.litertlm"));

    if (!IFileManager::Get().FileExists(*ModelPath))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("GenerateTest: model file not found at %s. See InoAgents.LoadEngineTest for the download URL."),
               *ModelPath);
        return;
    }

    UE_LOG(LogInoAgents, Log, TEXT("GenerateTest: starting"));
    UE_LOG(LogInoAgents, Log, TEXT("  Prompt: \"%s\""), *Prompt);
    UE_LOG(LogInoAgents, Log, TEXT("  Model:  %s"), *ModelPath);
    UE_LOG(LogInoAgents, Warning,
           TEXT("GenerateTest: the editor will freeze for several seconds (engine load + generation)."));
    GLog->Flush();

    // --- Keep UTF-8 buffers alive for the full C API call chain ---
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
    //
    // Passing nullptr for the config uses engine defaults. Session
    // ownership: the caller is responsible for litert_lm_session_delete().
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

    // --- Build InputData for the prompt ---
    //
    // Exactly one text input: a UTF-8 string with its byte length. The
    // buffer must stay alive until generate_content() returns, which it
    // does because PromptUtf8 is in the enclosing scope.
    InputData TextInput;
    TextInput.type = kInputText;
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
    TEXT("InoAgents.GenerateTest"),
    TEXT("Phase-1 smoke test: load engine, create a session, generate a "
         "completion for a text prompt, and clean up. Synchronous on the "
         "game thread; freezes the editor for ~5-10 seconds. Optionally "
         "takes a custom prompt as arguments: "
         "'InoAgents.GenerateTest Once upon a time'. Uses the raw "
         "generate_content API (no chat template) so instruction prompts "
         "get continued rather than answered — use the conversation API "
         "in a future milestone for instruction following."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunGenerateSmokeTest));


// ============================================================================
// Phase-1 conversation smoke test (milestone A)
// ============================================================================
//
// Console command: InoAgents.ConversationTest [optional prompt words...]
//
// Purpose: prove that the conversation API with chat template works. Unlike
// InoAgents.GenerateTest (which does raw next-token continuation), this
// wraps the user prompt in the model's instruction-following format via
// litert_lm_conversation_* APIs, so an instruction like
// "What is 2 plus 2?" should actually get an answer instead of being
// continued as text.
//
// Still synchronous, no streaming, no tools. Tool calling is the next
// milestone (B).
//
// Invoke:
//     InoAgents.ConversationTest
//     InoAgents.ConversationTest Write a haiku about Unreal Engine
// ============================================================================

static void RunConversationSmokeTest(const TArray<FString>& Args)
{
    // --- Prompt (default is a classic instruction) ---
    const FString Prompt = (Args.Num() > 0)
        ? FString::Join(Args, TEXT(" "))
        : FString(TEXT("What is 2 plus 2? Answer in one sentence."));

    // --- Resolve model path ---
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogInoAgents, Error, TEXT("ConversationTest: plugin not found"));
        return;
    }
    const FString BaseDir = Plugin->GetBaseDir();
    const FString ModelPath = FPaths::Combine(
        BaseDir, TEXT("Models"), TEXT("gemma-4-E2B-it.litertlm"));

    if (!IFileManager::Get().FileExists(*ModelPath))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ConversationTest: model file not found at %s"), *ModelPath);
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

    // Canonical system message from LiteRT-LM's own tests.
    const char* const SystemMessageJsonCStr =
        R"({"type":"text","text":"You are a helpful assistant. Answer concisely."})";

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
    //
    // Parameters:
    //   session_config = nullptr (use defaults)
    //   system_message_json = "You are a helpful assistant..."
    //   tools_json = nullptr (milestone B will add tools)
    //   messages_json = nullptr (no prior conversation history)
    //   enable_constrained_decoding = false (only needed with tools)
    LiteRtLmConversationConfig* ConvConfig = litert_lm_conversation_config_create(
        Engine,
        /* session_config              = */ nullptr,
        /* system_message_json         = */ SystemMessageJsonCStr,
        /* tools_json                  = */ nullptr,
        /* messages_json               = */ nullptr,
        /* enable_constrained_decoding = */ false);
    if (ConvConfig == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("ConversationTest: conversation_config_create returned NULL"));
        litert_lm_engine_delete(Engine);
        litert_lm_engine_settings_delete(Settings);
        return;
    }

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

    // --- Read response (owned by Response, copy immediately) ---
    const char* const ResponseJsonCStr = litert_lm_json_response_get_string(Response);
    if (ResponseJsonCStr == nullptr)
    {
        UE_LOG(LogInoAgents, Warning, TEXT("ConversationTest: response string is NULL"));
    }
    else
    {
        // Raw JSON for diagnosis — we'll parse it properly in the UE API layer.
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
    TEXT("InoAgents.ConversationTest"),
    TEXT("Phase-1 smoke test (milestone A): load engine, create a conversation "
         "with a system message, send one user message using the chat-template "
         "API, log the response JSON, and clean up. Synchronous. No tools, no "
         "streaming. Optionally takes a custom prompt as arguments: "
         "'InoAgents.ConversationTest Write a haiku about Unreal Engine'. "
         "Unlike InoAgents.GenerateTest, this applies the model's chat "
         "template internally — instruction prompts actually get answered."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunConversationSmokeTest));


// ============================================================================
// Phase-1 tool-calling smoke test (milestone B)
// ============================================================================
//
// Console command: InoAgents.ToolCallTest [optional prompt words...]
//
// Purpose: prove the agent loop — the model sees a prompt, decides to call a
// tool, we execute the tool locally, send the result back, and the model
// produces a final answer using that result. This is the headline feature
// of the InoAgents plugin.
//
// The test defines ONE in-process tool: get_current_time, which takes no
// arguments and returns FDateTime::Now() as a string. The default prompt is
// "What time is it right now?" which should reliably trigger the tool on an
// instruction-tuned Gemma 4 model.
//
// enable_constrained_decoding = true is required for reliable tool calling.
// It routes the model's output through libGemmaModelConstraintProvider.dll
// (which is exactly why that DLL is a required runtime sibling of LiteRtLm.dll)
// so that when a tool call is expected, the sampler is constrained to emit
// syntactically valid function call JSON.
//
// Still synchronous and still on the game thread. Editor will freeze for
// ~5-15 seconds (engine load + two generation passes).
//
// Invoke:
//     InoAgents.ToolCallTest
//     InoAgents.ToolCallTest Please tell me the current time.
// ============================================================================

namespace
{
    /**
     * Parse a JSON string into an FJsonObject. Logs and returns nullptr on
     * failure.
     */
    TSharedPtr<FJsonObject> ParseJsonObjectOrLog(const FString& Json, const TCHAR* Context)
    {
        TSharedPtr<FJsonObject> Obj;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("%s: failed to parse JSON: %s"),
                   Context, *Json);
            return nullptr;
        }
        return Obj;
    }

    /**
     * Extract the first tool call from an assistant response JSON object, if
     * present. Returns false if the response has no tool_calls field (i.e.
     * the model answered in plain text).
     */
    bool TryExtractFirstToolCall(const TSharedPtr<FJsonObject>& ResponseObj,
                                 FString& OutToolName,
                                 TSharedPtr<FJsonObject>& OutArgumentsObj)
    {
        const TArray<TSharedPtr<FJsonValue>>* ToolCallsArrayPtr = nullptr;
        if (!ResponseObj->TryGetArrayField(TEXT("tool_calls"), ToolCallsArrayPtr)
            || ToolCallsArrayPtr == nullptr
            || ToolCallsArrayPtr->Num() == 0)
        {
            return false;
        }

        const TSharedPtr<FJsonValue>& FirstToolCallValue = (*ToolCallsArrayPtr)[0];
        if (!FirstToolCallValue.IsValid()
            || FirstToolCallValue->Type != EJson::Object)
        {
            return false;
        }
        const TSharedPtr<FJsonObject>& FirstToolCallObj = FirstToolCallValue->AsObject();

        const TSharedPtr<FJsonObject>* FunctionObjPtr = nullptr;
        if (!FirstToolCallObj->TryGetObjectField(TEXT("function"), FunctionObjPtr)
            || FunctionObjPtr == nullptr)
        {
            return false;
        }
        const TSharedPtr<FJsonObject>& FunctionObj = *FunctionObjPtr;

        if (!FunctionObj->TryGetStringField(TEXT("name"), OutToolName))
        {
            return false;
        }

        // arguments may be either a JSON object or absent (for zero-arg tools)
        const TSharedPtr<FJsonObject>* ArgumentsObjPtr = nullptr;
        if (FunctionObj->TryGetObjectField(TEXT("arguments"), ArgumentsObjPtr)
            && ArgumentsObjPtr != nullptr)
        {
            OutArgumentsObj = *ArgumentsObjPtr;
        }
        else
        {
            OutArgumentsObj = MakeShared<FJsonObject>();
        }

        return true;
    }

    /**
     * Pretty-extract the concatenated plain-text content from an assistant
     * response for logging purposes. Returns empty string if the response has
     * no text content.
     */
    FString ExtractAssistantText(const TSharedPtr<FJsonObject>& ResponseObj)
    {
        const TArray<TSharedPtr<FJsonValue>>* ContentArrayPtr = nullptr;
        if (!ResponseObj->TryGetArrayField(TEXT("content"), ContentArrayPtr)
            || ContentArrayPtr == nullptr)
        {
            return FString();
        }

        FString Result;
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
                if (!Result.IsEmpty())
                {
                    Result += TEXT(" ");
                }
                Result += PartText;
            }
        }
        return Result;
    }

    /**
     * The one local tool this smoke test exposes to the model. Adds two
     * integers from the model's parsed arguments and returns the sum.
     *
     * Returns true on success, false if the arguments object did not
     * contain parseable integer fields 'a' and 'b'.
     *
     * Why this tool and not get_current_time: Gemma 4 E2B is RLHF-trained
     * to refuse questions about the current time ("I do not have access
     * to real-time information"), and that reflex is stronger than its
     * willingness to use a tool result. A pure math function has no such
     * training baggage, so it's a cleaner test of whether tool results
     * actually round-trip into the model's response.
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

        // Models sometimes emit integer args as JSON numbers and sometimes
        // as strings containing integers. Accept both.
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
    // --- Prompt ---
    // Default is a plain arithmetic question. Small instruction-tuned models
    // have no reflex around math questions (unlike "what time is it?" which
    // Gemma 4 is RLHFed to refuse) so this is a much cleaner test of the
    // full agent loop: user question → tool call → result → final answer.
    const FString Prompt = (Args.Num() > 0)
        ? FString::Join(Args, TEXT(" "))
        : FString(TEXT("What is 27 plus 15?"));

    // --- Resolve model path ---
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogInoAgents, Error, TEXT("ToolCallTest: plugin not found"));
        return;
    }
    const FString BaseDir = Plugin->GetBaseDir();
    const FString ModelPath = FPaths::Combine(
        BaseDir, TEXT("Models"), TEXT("gemma-4-E2B-it.litertlm"));

    if (!IFileManager::Get().FileExists(*ModelPath))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ToolCallTest: model file not found at %s"), *ModelPath);
        return;
    }

    UE_LOG(LogInoAgents, Log, TEXT("ToolCallTest: starting"));
    UE_LOG(LogInoAgents, Log, TEXT("  Prompt: \"%s\""), *Prompt);
    UE_LOG(LogInoAgents, Warning,
           TEXT("ToolCallTest: editor will freeze for ~5-15 seconds (engine + 2 generation passes)."));
    GLog->Flush();

    // --- Static JSON strings (owned here, lifetimes simple) ---
    //
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

    const TSharedPtr<FJsonObject> Response1Obj = ParseJsonObjectOrLog(Response1Json, TEXT("ToolCallTest round 1"));
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
    const bool bHasToolCall = TryExtractFirstToolCall(Response1Obj, ToolName, ArgumentsObj);

    if (!bHasToolCall)
    {
        // The model answered in plain text without calling a tool. This is
        // a valid outcome for small models, worth logging clearly.
        const FString AssistantText = ExtractAssistantText(Response1Obj);
        UE_LOG(LogInoAgents, Warning,
               TEXT("ToolCallTest: model did NOT request a tool call. "
                    "Plain-text reply: \"%s\""),
               *AssistantText);
        UE_LOG(LogInoAgents, Warning,
               TEXT("ToolCallTest: this is not a failure — just means Gemma 4 E2B "
                    "chose not to use the tool for this prompt. Try a more direct "
                    "instruction, e.g. 'Call the get_current_time tool.'"));

        litert_lm_json_response_delete(Response1);
        litert_lm_conversation_delete(Conversation);
        litert_lm_conversation_config_delete(ConvConfig);
        litert_lm_engine_delete(Engine);
        litert_lm_engine_settings_delete(Settings);
        return;
    }

    UE_LOG(LogInoAgents, Log, TEXT("ToolCallTest: model called tool \"%s\""), *ToolName);

    // --- Execute the tool locally ---
    //
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
            // Emit an error string as a quoted JSON string value.
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
    //
    // Shape (from runtime/conversation/model_data_processor/gemma4_data_processor_test.cc,
    // specifically MessageToTemplateInputWithToolResponseWithNonObjectValue):
    // tool_response.value may be a scalar — a number or a string — and the
    // data processor renders it as "<tool_name>{value:<ctrl46><scalar><ctrl46>}"
    // in the text the model sees. For a single-result tool like add_numbers,
    // a bare scalar is cleaner than an object like {"sum": 42} because it
    // avoids introducing an arbitrary field name.
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

    const TSharedPtr<FJsonObject> Response2Obj = ParseJsonObjectOrLog(Response2Json, TEXT("ToolCallTest round 2"));
    if (Response2Obj.IsValid())
    {
        const FString FinalText = ExtractAssistantText(Response2Obj);
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

void FInoAgentsModule::ShutdownModule()
{
    // Unload in reverse order of dependency: the main DLL first, then the
    // sibling it depends on.
    if (LiteRtLmHandle)
    {
        FPlatformProcess::FreeDllHandle(LiteRtLmHandle);
        LiteRtLmHandle = nullptr;
    }
    if (GemmaConstraintProviderHandle)
    {
        FPlatformProcess::FreeDllHandle(GemmaConstraintProviderHandle);
        GemmaConstraintProviderHandle = nullptr;
    }
}

IMPLEMENT_MODULE(FInoAgentsModule, InoAgents)
