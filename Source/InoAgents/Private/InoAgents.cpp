// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoAgents.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

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
