// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// =====================================================================
// Ino.NeuTTS.LoadTest — subsystem-level end-to-end load smoke test.
// =====================================================================
//
// Drives `UInoNeuTTSSubsystem::LoadModelAsync` against the configured
// backbone (.litertlm) + decoder (.tflite) entries in Project Settings
// → Plugins → InoNeuTTS. Logs:
//
//   - Each download-progress tick (throttled to keep the log readable)
//   - The terminal OnLoaded result + total wall-clock time
//
// Requires PIE — the subsystem is a UGameInstanceSubsystem, so a
// game instance must be alive. Run from the Output Log command bar
// once you've started PIE.
//
// Usage:
//   Ino.NeuTTS.LoadTest [backbone_name] [decoder_name]
//
// Empty / omitted names → uses the first entry of each registry array.
// Names match case-insensitively against DisplayName OR LocalFileName.

#include "InoNeuTTSLoadTest.h"

#include "NeuTTS/InoNeuTTSSubsystem.h"

#include "InoAgentsLog.h"
#include "InoSmokeTestCommon.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

void UInoNeuTTSLoadTestObserver::Begin(
    UInoNeuTTSSubsystem* InSubsys, const FInoNeuTTSConfig& InConfig)
{
    Subsys        = InSubsys;
    TStartSeconds = FPlatformTime::Seconds();

    FInoNeuTTSDownloadProgressDelegate ProgressCb;
    ProgressCb.BindDynamic(this, &UInoNeuTTSLoadTestObserver::HandleDownloadProgress);

    FInoNeuTTSLoadedDelegate LoadedCb;
    LoadedCb.BindDynamic(this, &UInoNeuTTSLoadTestObserver::HandleLoaded);

    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][LoadTest] dispatching LoadModelAsync (backbone='%s', decoder='%s')"),
        *InConfig.BackboneModelName, *InConfig.DecoderModelName);

    Subsys->LoadModelAsync(InConfig, LoadedCb, ProgressCb);
}

void UInoNeuTTSLoadTestObserver::HandleDownloadProgress(const FInoDownloadProgress& Progress)
{
    // Throttle — fire one log line every ~16 progress ticks (~1Hz for a
    // typical 60Hz progress stream). Always log the first and the last
    // tick so the user sees clear start/end markers even on small files.
    ++ProgressLogStride;
    const bool bIsBoundary = (Progress.OverallProgressPercent <= 0.5f)
                          || (Progress.OverallProgressPercent >= 99.5f);
    if (bIsBoundary || (ProgressLogStride % 16) == 0)
    {
        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][LoadTest] dl: file %d/%d '%s' %.1f%% (file %lld/%lld, overall %.1f%%, %.2f MB/s)"),
            Progress.CurrentFileIndex + 1, Progress.TotalFiles,
            *Progress.CurrentFileName,
            Progress.ProgressPercent,
            Progress.BytesReceived, Progress.TotalBytes,
            Progress.OverallProgressPercent,
            Progress.BytesPerSecond / (1024.0f * 1024.0f));
    }
}

void UInoNeuTTSLoadTestObserver::HandleLoaded(bool bSuccess, FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - TStartSeconds;
    if (bSuccess)
    {
        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][LoadTest] === PASS === load OK in %.2f s — model ready for synth."),
            Elapsed);
        if (Subsys)
        {
            UE_LOG(LogInoAgents, Log,
                TEXT("[NeuTTS][LoadTest] subsystem state: IsModelLoaded=%d, HasActiveVoice=%d, SampleRate=%d"),
                Subsys->IsModelLoaded() ? 1 : 0,
                Subsys->HasActiveVoice() ? 1 : 0,
                Subsys->GetSampleRate());
        }
    }
    else
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][LoadTest] === FAIL === %s (after %.2f s)"),
            *ErrorMessage, Elapsed);
    }

    // Self-unroot so GC can collect us on the next pass.
    RemoveFromRoot();
}

namespace
{
    static void RunLoadTest(const TArray<FString>& Args)
    {
        UInoNeuTTSSubsystem* Subsys =
            InoSmokeTest::FindGameInstanceSubsystem<UInoNeuTTSSubsystem>();
        if (!Subsys)
        {
            UE_LOG(LogInoAgents, Error,
                TEXT("[NeuTTS][LoadTest] No UInoNeuTTSSubsystem available — start PIE first."));
            return;
        }
        if (Subsys->IsModelLoaded())
        {
            UE_LOG(LogInoAgents, Warning,
                TEXT("[NeuTTS][LoadTest] Model already loaded — call Ino.NeuTTS.* tests against it ")
                TEXT("or run Subsys->UnloadModel via Blueprint before retrying."));
            return;
        }

        FInoNeuTTSConfig Config;
        if (Args.Num() >= 1) Config.BackboneModelName = Args[0];
        if (Args.Num() >= 2) Config.DecoderModelName  = Args[1];

        UInoNeuTTSLoadTestObserver* Observer = NewObject<UInoNeuTTSLoadTestObserver>();
        Observer->AddToRoot();  // keep alive across the async load
        Observer->Begin(Subsys, Config);
    }

    static FAutoConsoleCommand GLoadTestCmd(
        TEXT("Ino.NeuTTS.LoadTest"),
        TEXT("End-to-end load via UInoNeuTTSSubsystem — downloads backbone+decoder if needed, ")
        TEXT("constructs the runner, optionally warmup. Args: [backbone_name] [decoder_name]. ")
        TEXT("Requires PIE."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunLoadTest));
}
