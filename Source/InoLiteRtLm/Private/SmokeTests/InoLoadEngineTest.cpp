// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.LoadEngineTest
// ============================================================================
//
// Phase-1 smoke test: synchronously load and destroy a LiteRT-LM engine from
// the plugin's default model file. Calls:
//
//   litert_lm_engine_settings_create
//     -> litert_lm_engine_create       (the slow step — actually parses weights)
//     -> litert_lm_engine_delete
//     -> litert_lm_engine_settings_delete
//
// Logs wall-clock duration for each step. Does NOT create a session, does
// NOT generate text — that is the job of Ino.GenerateTest and beyond.
//
// Runs synchronously on the game thread. Editor will freeze for approximately
// 2-30 seconds depending on disk state (memory-mapped file load is fast when
// the OS page cache is warm, slower from cold).
//
// Invoke:
//     Ino.LoadEngineTest
// ============================================================================

#include "InoAgentsLog.h"
#include "InoSmokeTestCommon.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

#include "litert/lm/engine.h"

static void RunLoadEngineSmokeTest(const TArray<FString>& /*Args*/)
{
    // --- Resolve model path ---
    const FString ModelPath = InoSmokeTest::ResolveDefaultModelPath();
    if (ModelPath.IsEmpty())
    {
        return;
    }

    const int64 FileSizeBytes = IFileManager::Get().FileSize(*ModelPath);
    const double FileSizeGB = static_cast<double>(FileSizeBytes) / (1024.0 * 1024.0 * 1024.0);

    UE_LOG(LogInoAgents, Log, TEXT("LoadEngineTest: starting synchronous engine load"));
    UE_LOG(LogInoAgents, Log, TEXT("  Model path : %s"), *ModelPath);
    UE_LOG(LogInoAgents, Log, TEXT("  Model size : %.2f GB (%lld bytes)"), FileSizeGB, FileSizeBytes);
    UE_LOG(LogInoAgents, Log, TEXT("  Backend    : cpu"));
    UE_LOG(LogInoAgents, Warning,
           TEXT("LoadEngineTest: the editor will freeze for the next 2-30 seconds. "
                "This is expected for phase 1."));

    // Force a log flush so the warning actually appears before the freeze.
    GLog->Flush();

    // --- UTF-8 buffer for the model path ---
    // FTCHARToUTF8's internal buffer must outlive every call that uses it.
    const FTCHARToUTF8 ModelPathUtf8(*ModelPath);
    const char* const ModelPathCStr = ModelPathUtf8.Get();

    // --- Create engine settings ---
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

    // --- Create the engine (slow: parses weights) ---
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

    // --- Cleanup (reverse order of creation) ---
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

static FAutoConsoleCommand GLoadEngineTestCommand(
    TEXT("Ino.LoadEngineTest"),
    TEXT("Phase-1 smoke test: synchronously load and destroy a LiteRT-LM engine "
         "(resolved via <ProjectPersistentDownloadDir>/ino-agents/lite-rt-lm/ then "
         "the legacy <InoAgents>/LiteRTLM/ drop) on the game thread. Freezes the "
         "editor for 2-30 s. No session or generation; this only proves the "
         "engine can be constructed and destroyed cleanly."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunLoadEngineSmokeTest));
