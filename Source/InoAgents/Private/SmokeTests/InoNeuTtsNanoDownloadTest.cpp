// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.NeuTtsNano.DownloadTest (Milestone 2 verification)
// ============================================================================
//
// Exercises UInoNeuTtsNanoSubsystem::LoadModelAsync's download half:
//
//   1. Grab the subsystem from the current game instance (requires PIE).
//   2. Build a FInoNeuTtsNanoModelConfig pointing at the Q4 variant.
//   3. Bind OnDownloadProgress + the one-shot OnLoaded delegate.
//   4. Call LoadModelAsync:
//        - On warm cache: the loader dispatches immediately with
//          bSuccess=true and the progress delegate never fires.
//        - On cold cache: downloads ~978 MB (backbone + codec),
//          logs progress at ~1 Hz, dispatches the loader stub on
//          completion.
//   5. On success, file-stat both expected files and log their sizes.
//   6. Unload.
//
// Runs non-blocking: the command returns immediately; async callbacks
// fire on the game thread and log progressively.
//
// Invoke (PIE required):
//     Ino.NeuTtsNano.DownloadTest
// ============================================================================

#include "InoNeuTtsNanoDownloadTest.h"

#include "InoAgentsLog.h"
#include "InoAgentsSettings.h"
#include "InoSmokeTestCommon.h"
#include "NeuTtsNano/InoNeuTtsNanoSubsystem.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

void UInoNeuTtsNanoDownloadTestObserver::HandleProgress(
    float Percent, int64 BytesReceived, int64 TotalBytes, bool bCompleted)
{
    // Always log the terminal completion tick regardless of throttle —
    // useful diagnostic for confirming bCompleted fires exactly once
    // per successful download.
    if (bCompleted)
    {
        const double MbReceived = (double)BytesReceived / (1024.0 * 1024.0);
        UE_LOG(LogInoAgents, Log,
               TEXT("NeuTtsNano.DownloadTest: DOWNLOAD COMPLETE (%.1f MB, 100%%); ")
               TEXT("ThreadPool load dispatching next."),
               MbReceived);
        return;
    }

    // Throttle intermediate progress logs to ~1 Hz so the Output Log
    // doesn't drown in 100+ lines per file.
    const double Now = FPlatformTime::Seconds();
    if (Now - LastProgressLogTime < 1.0)
    {
        return;
    }
    LastProgressLogTime = Now;

    const double MbReceived = (double)BytesReceived / (1024.0 * 1024.0);
    if (TotalBytes > 0)
    {
        const double MbTotal = (double)TotalBytes / (1024.0 * 1024.0);
        UE_LOG(LogInoAgents, Log,
               TEXT("NeuTtsNano.DownloadTest: %.1f%% (%.1f / %.1f MB)"),
               Percent, MbReceived, MbTotal);
    }
    else
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("NeuTtsNano.DownloadTest: %.1f%% (%.1f MB; total unknown)"),
               Percent, MbReceived);
    }
}

void UInoNeuTtsNanoDownloadTestObserver::HandleLoaded(
    bool bSuccess, FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;

    if (!bSuccess)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("NeuTtsNano.DownloadTest: FAILED after %.2f s: %s"),
               Elapsed, *ErrorMessage);
        RemoveFromRoot();
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano.DownloadTest: load-callback fired in %.2f s, bSuccess=true"),
           Elapsed);

    // File-stat check: both expected files should exist on disk now.
    const UInoAgentsSettings* Settings = UInoAgentsSettings::Get();
    const FInoNeuTtsNanoModelEntry* Entry =
        Settings ? Settings->FindNeuTtsNanoModel(Config.Variant) : nullptr;
    if (Entry != nullptr)
    {
        const FString Dir = NeuTtsNanoResolveModelDir(Config.Variant);
        IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

        auto StatLog = [&](const TCHAR* Label, const FString& FileName)
        {
            const FString Path = FPaths::Combine(Dir, FileName);
            const bool bExists = PF.FileExists(*Path);
            const int64 Size   = bExists ? PF.FileSize(*Path) : -1;
            if (bExists && Size > 0)
            {
                UE_LOG(LogInoAgents, Log,
                       TEXT("  [%s] %s — %.1f MB"),
                       Label, *Path, (double)Size / (1024.0 * 1024.0));
            }
            else
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("  [%s] %s — MISSING or empty"),
                       Label, *Path);
            }
        };
        StatLog(TEXT("backbone"), Entry->BackboneFileName);
        StatLog(TEXT("codec"),    Entry->CodecFileName);
    }

    // Verify the subsystem's loaded-state accessor. In Milestone 2 the
    // DispatchLoadWorker stub just sets bModelLoaded=true; Milestone 3
    // upgrades this to reflect actual llama_model/llama_context state.
    if (Subsystem != nullptr)
    {
        const bool bLoaded = Subsystem->IsModelLoaded();
        UE_LOG(LogInoAgents, Log,
               TEXT("NeuTtsNano.DownloadTest: IsModelLoaded()=%s after callback"),
               bLoaded ? TEXT("true") : TEXT("false"));
        Subsystem->UnloadModel();
        UE_LOG(LogInoAgents, Log,
               TEXT("NeuTtsNano.DownloadTest: UnloadModel() called; IsModelLoaded()=%s"),
               Subsystem->IsModelLoaded() ? TEXT("true") : TEXT("false"));
    }

    UE_LOG(LogInoAgents, Log, TEXT("NeuTtsNano.DownloadTest: DONE"));
    RemoveFromRoot();
}

static void RunNeuTtsNanoDownloadTest(const TArray<FString>& /*Args*/)
{
    UInoNeuTtsNanoSubsystem* Subsys = InoSmokeTest::FindNeuTtsNanoSubsystem();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("NeuTtsNano.DownloadTest: no UInoNeuTtsNanoSubsystem found. "
                    "Enter PIE first — GameInstanceSubsystems are created by "
                    "the active game instance."));
        return;
    }

    if (Subsys->IsModelLoaded())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("NeuTtsNano.DownloadTest: a model is already loaded. "
                    "Unloading first so the test runs from a clean state."));
        Subsys->UnloadModel();
    }

    FInoNeuTtsNanoModelConfig Config;
    Config.Variant = EInoNeuTtsNanoBackboneVariant::Q4;

    auto* Observer = NewObject<UInoNeuTtsNanoDownloadTestObserver>();
    Observer->StartTime           = FPlatformTime::Seconds();
    Observer->LastProgressLogTime = 0.0;   // force first progress log
    Observer->Subsystem           = Subsys;
    Observer->Config              = Config;
    Observer->AddToRoot();

    // Per-call delegates — OnDownloadProgress fires during download
    // only, OnLoaded fires once at terminal completion. Single-cast
    // dynamic so we bind one observer handler for each and hand them
    // to LoadModelAsync as parameters.
    FOnInoNeuTtsNanoDownloadProgress ProgressDelegate;
    ProgressDelegate.BindDynamic(
        Observer, &UInoNeuTtsNanoDownloadTestObserver::HandleProgress);

    FOnInoNeuTtsNanoModelLoaded LoadedDelegate;
    LoadedDelegate.BindDynamic(
        Observer, &UInoNeuTtsNanoDownloadTestObserver::HandleLoaded);

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano.DownloadTest: calling LoadModelAsync (non-blocking). "
                "On cold cache this downloads ~978 MB and may take 1-3 minutes."));

    Subsys->LoadModelAsync(Config, ProgressDelegate, LoadedDelegate);

    UE_LOG(LogInoAgents, Log,
           TEXT("NeuTtsNano.DownloadTest: LoadModelAsync returned synchronously."));
}

static FAutoConsoleCommand GNeuTtsNanoDownloadTestCmd(
    TEXT("Ino.NeuTtsNano.DownloadTest"),
    TEXT("Milestone 2 smoke test: kicks off UInoNeuTtsNanoSubsystem::LoadModelAsync "
         "for the Q4 variant, logs OnDownloadProgress at ~1 Hz, verifies both "
         "files land on disk in PersistentDownloadDir/InoAgents/Models/NeuTtsNano/q4/ "
         "after the loaded delegate fires. PIE required."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunNeuTtsNanoDownloadTest));
