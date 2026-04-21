// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.Chatterbox.SubsystemSynthTest
// ============================================================================
//
// Phase D Commit 3 smoke test: exercises UInoChatterboxTtsSubsystem's
// full async API end-to-end through PIE (subsystem is a
// UGameInstanceSubsystem, so no active game = no subsystem = test
// bails with a clear error).
//
// Flow:
//   1. Grab the subsystem from the current GameInstance.
//   2. Build FInoChatterboxModelConfig{Variant = Q4F16 (or user arg)}.
//   3. Resolve default_voice.wav inside the staged variant dir — the
//      same file the existing native-layer Ino.Chatterbox.SynthTest
//      uses. Produced by setup-chatterbox.ps1 -IncludeDefaultVoice.
//   4. Call LoadModelsAsync with a dynamic-delegate callback.
//   5. OnLoaded success → build FInoChatterboxVoice{WavFilePath=...} +
//      FInoChatterboxSynthesisOptions, call SynthesizeAsync with a
//      second dynamic-delegate callback.
//   6. OnSynthComplete success → run waveform sanity checks (NaN /
//      inf / range), save to <Saved>/Chatterbox/subsystem_synth_test.wav
//      via InoChatterbox::WriteMonoInt16Wav, log per-stage timings.
//   7. UnloadModels + RemoveFromRoot so the observer + its held
//      references can be collected.
//
// Non-blocking: LoadModelsAsync and SynthesizeAsync both return
// immediately; the editor stays responsive during the ~1-5 s load
// and the ~1-10 s synthesis.
//
// The existing native-layer Ino.Chatterbox.SynthTest stays — it
// exercises FInoChatterboxRunner directly without touching the
// subsystem, so a regression in either layer can be diagnosed
// without the other being a suspect. Same split the LiteRT-LM side
// uses for its Phase-1 vs UE-API smoke tests.
//
// Invoke:
//     Ino.Chatterbox.SubsystemSynthTest [variant] [max_new_tokens] [text...]
// Defaults:
//     variant        = q4f16
//     max_new_tokens = 512
//     text           = "This is a subsystem synthesis smoke test."
// ============================================================================

#include "InoChatterboxSubsystemTest.h"

#include "InoAgentsLog.h"
#include "InoChatterboxAudioIO.h"
#include "InoSmokeTestCommon.h"
#include "Chatterbox/InoChatterboxTtsSubsystem.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

// ----------------------------------------------------------------------------
// OnLoaded handler
// ----------------------------------------------------------------------------

void UInoChatterboxSubsystemTestObserver::HandleLoaded(
    bool bSuccess, FString ErrorMessage)
{
    const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

    if (!bSuccess)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("SubsystemSynthTest: LoadModelsAsync FAILED after %.1f ms: %s"),
               ElapsedMs, *ErrorMessage);
        RemoveFromRoot();
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("SubsystemSynthTest: LoadModelsAsync SUCCESS in %.1f ms, ")
           TEXT("IsModelsLoaded()=%s, variant=%d"),
           ElapsedMs,
           (Subsystem && Subsystem->IsModelsLoaded()) ? TEXT("true") : TEXT("false"),
           Subsystem ? (int32)Subsystem->GetLoadedVariant() : -1);

    if (!IFileManager::Get().FileExists(*VoiceWavPath))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("SubsystemSynthTest: reference voice WAV not found at %s. ")
               TEXT("Run Plugins/InoAgents/Chatterbox/scripts/setup-chatterbox.ps1 -IncludeDefaultVoice ")
               TEXT("or point PromptText[0] at your own 24 kHz mono WAV."),
               *VoiceWavPath);
        if (Subsystem) { Subsystem->UnloadModels(); }
        RemoveFromRoot();
        return;
    }

    // Build the synth call. AR loop timing starts when SynthesizeAsync
    // returns → we re-base StartTime for the next elapsed measurement.
    StartTime = FPlatformTime::Seconds();

    FInoChatterboxVoice Voice;
    Voice.WavFilePath = VoiceWavPath;

    FInoChatterboxSynthesisOptions Options;
    Options.MaxNewTokens      = MaxNewTokens;
    Options.RepetitionPenalty = 1.2f;

    FOnInoChatterboxSynthesisComplete OnComplete;
    OnComplete.BindDynamic(this, &UInoChatterboxSubsystemTestObserver::HandleSynthComplete);

    UE_LOG(LogInoAgents, Log,
           TEXT("SubsystemSynthTest: SynthesizeAsync dispatching — voice=%s, text=\"%s\", max=%d"),
           *VoiceWavPath, *PromptText, MaxNewTokens);

    Subsystem->SynthesizeAsync(PromptText, Voice, Options, OnComplete);
}

// ----------------------------------------------------------------------------
// OnSynthComplete handler
// ----------------------------------------------------------------------------

void UInoChatterboxSubsystemTestObserver::HandleSynthComplete(
    bool bSuccess,
    FInoChatterboxSynthesisResult Result,
    FString ErrorMessage)
{
    const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

    if (!bSuccess)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("SubsystemSynthTest: SynthesizeAsync FAILED after %.1f ms: %s"),
               ElapsedMs, *ErrorMessage);
    }
    else
    {
        // -------- Waveform sanity scan --------
        // Matches the checks in the native-layer SynthTest so PASS/FAIL
        // criteria are consistent across both smoke tests.
        const int32 N = Result.AudioSamples.Num();
        int32  NumNaN      = 0;
        int32  NumInf      = 0;
        int32  NumOutRange = 0;
        float  MinV        = FLT_MAX;
        float  MaxV        = -FLT_MAX;
        for (int32 i = 0; i < N; ++i)
        {
            const float V = Result.AudioSamples[i];
            if (FMath::IsNaN(V))       { ++NumNaN; }
            else if (!FMath::IsFinite(V)) { ++NumInf; }
            else if (V < -1.0f || V > 1.0f) { ++NumOutRange; }
            if (V < MinV) { MinV = V; }
            if (V > MaxV) { MaxV = V; }
        }

        const float DurationSec =
            (N > 0 && Result.SampleRate > 0)
            ? (float)N / (float)Result.SampleRate
            : 0.0f;

        UE_LOG(LogInoAgents, Log,
               TEXT("SubsystemSynthTest: SynthesizeAsync SUCCESS in %.1f ms. ")
               TEXT("Samples=%d (%.2f s @ %d Hz), tokens=%d, stop=%s. ")
               TEXT("NaN=%d Inf=%d OutOfRange=%d Min=%.3f Max=%.3f."),
               ElapsedMs, N, DurationSec, Result.SampleRate,
               Result.NumGeneratedTokens,
               Result.bHitStopToken ? TEXT("yes") : TEXT("no"),
               NumNaN, NumInf, NumOutRange, MinV, MaxV);

        UE_LOG(LogInoAgents, Log,
               TEXT("SubsystemSynthTest: per-stage: encoder=%.1f ms embed=%.1f ms LM=%.1f ms decoder=%.1f ms total=%.1f ms"),
               Result.EncoderMs, Result.EmbedTotalMs, Result.LanguageModelMs,
               Result.DecoderMs, Result.TotalElapsedMs);

        // -------- Save WAV --------
        if (!OutputWavPath.IsEmpty() && N > 0)
        {
            // Ensure the parent directory exists (FFileHelper doesn't
            // create directories).
            const FString OutputDir = FPaths::GetPath(OutputWavPath);
            if (!OutputDir.IsEmpty())
            {
                IFileManager::Get().MakeDirectory(*OutputDir, /*Tree=*/ true);
            }

            const bool bWrote = InoChatterbox::WriteMonoInt16Wav(
                OutputWavPath,
                MakeArrayView(Result.AudioSamples),
                Result.SampleRate);

            if (bWrote)
            {
                UE_LOG(LogInoAgents, Log,
                       TEXT("SubsystemSynthTest: wrote %s"), *OutputWavPath);
            }
            else
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("SubsystemSynthTest: FAILED to write %s"), *OutputWavPath);
            }
        }

        const bool bWaveformOk =
            (N > 0) && (NumNaN == 0) && (NumInf == 0) && (NumOutRange == 0);
        UE_LOG(LogInoAgents, Log,
               TEXT("SubsystemSynthTest: %s"),
               bWaveformOk ? TEXT("PASS") : TEXT("FAIL (waveform failed sanity checks)"));
    }

    // -------- Teardown --------
    if (Subsystem)
    {
        Subsystem->UnloadModels();
        UE_LOG(LogInoAgents, Log,
               TEXT("SubsystemSynthTest: unloaded; IsModelsLoaded()=%s. DONE."),
               Subsystem->IsModelsLoaded() ? TEXT("true") : TEXT("false"));
    }

    RemoveFromRoot();
}

// ----------------------------------------------------------------------------
// Console command entry point
// ----------------------------------------------------------------------------

namespace
{
    void RunSubsystemSynthTest(const TArray<FString>& Args)
    {
        // -------- Arg parsing (same shape as the native SynthTest) --------
        //   Args[0]        : variant            (default: q4f16)
        //   Args[1] digits : max_new_tokens     (default: 512)
        //   Args[1..] text : prompt
        FString VariantStr = (Args.Num() > 0) ? Args[0] : FString(TEXT("q4f16"));

        int32 MaxNewTokens = 512;
        int32 TextStartIdx = 1;
        if (Args.Num() >= 2 && !Args[1].IsEmpty() && FChar::IsDigit(Args[1][0]))
        {
            MaxNewTokens = FMath::Clamp(FCString::Atoi(*Args[1]), 1, 1024);
            TextStartIdx = 2;
        }

        FString PromptText;
        for (int32 i = TextStartIdx; i < Args.Num(); ++i)
        {
            if (!PromptText.IsEmpty()) { PromptText += TEXT(" "); }
            PromptText += Args[i];
        }
        if (PromptText.IsEmpty())
        {
            PromptText = TEXT("This is a subsystem synthesis smoke test.");
        }

        EInoChatterboxVariant VariantEnum;
        if (!ChatterboxVariantFromString(VariantStr, VariantEnum))
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("SubsystemSynthTest: unknown variant '%s' ")
                   TEXT("(expected q4f16 / fp16 / q4 / fp32 / quantized)"),
                   *VariantStr);
            return;
        }

        // -------- Subsystem lookup --------
        UInoChatterboxTtsSubsystem* Subsys = InoSmokeTest::FindChatterboxSubsystem();
        if (Subsys == nullptr)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("SubsystemSynthTest: could not find UInoChatterboxTtsSubsystem. ")
                   TEXT("This usually means there's no active game instance — ")
                   TEXT("run the test from PIE."));
            return;
        }

        if (Subsys->IsModelsLoaded())
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("SubsystemSynthTest: models are already loaded (variant=%d); ")
                   TEXT("unloading first so the test runs from a clean state."),
                   (int32)Subsys->GetLoadedVariant());
            Subsys->UnloadModels();
        }

        // -------- Resolve paths --------
        // Use the public helper rather than duplicating it in an
        // anonymous namespace — the pre-existing InoChatterboxTest.cpp
        // already has its own file-local ResolveChatterboxDir, and UBT's
        // unity build merges the two files' anonymous namespaces into one
        // TU, so a duplicated definition here collides at link time.
        const FString VariantDir  = ChatterboxResolveVariantDir(VariantEnum);
        const FString VoiceWav    = FPaths::Combine(VariantDir, TEXT("default_voice.wav"));
        const FString OutputWav   = FPaths::Combine(
            FPaths::ProjectSavedDir(), TEXT("Chatterbox"),
            TEXT("subsystem_synth_test.wav"));

        // -------- Observer --------
        auto* Observer = NewObject<UInoChatterboxSubsystemTestObserver>();
        Observer->StartTime     = FPlatformTime::Seconds();
        Observer->Subsystem     = Subsys;
        Observer->VoiceWavPath  = VoiceWav;
        Observer->PromptText    = PromptText;
        Observer->OutputWavPath = OutputWav;
        Observer->MaxNewTokens  = MaxNewTokens;
        Observer->AddToRoot();

        FInoChatterboxModelConfig Config;
        Config.Variant = VariantEnum;

        FOnInoChatterboxModelsLoaded OnLoaded;
        OnLoaded.BindDynamic(Observer, &UInoChatterboxSubsystemTestObserver::HandleLoaded);

        UE_LOG(LogInoAgents, Log,
               TEXT("SubsystemSynthTest: starting. variant=%s max_new_tokens=%d prompt=\"%s\""),
               *VariantStr, MaxNewTokens, *PromptText);
        UE_LOG(LogInoAgents, Log,
               TEXT("SubsystemSynthTest: LoadModelsAsync dispatching (non-blocking)."));

        Subsys->LoadModelsAsync(Config, OnLoaded);
    }

    FAutoConsoleCommand GChatterboxSubsystemSynthTestCmd(
        TEXT("Ino.Chatterbox.SubsystemSynthTest"),
        TEXT("Phase D Commit 3 smoke test: end-to-end subsystem flow — ")
        TEXT("LoadModelsAsync -> SynthesizeAsync -> WAV write -> UnloadModels. ")
        TEXT("Requires PIE (subsystem is game-instance-scoped). ")
        TEXT("Needs default_voice.wav staged (setup-chatterbox.ps1 -IncludeDefaultVoice). ")
        TEXT("Args: [variant] [max_new_tokens] [text...]. ")
        TEXT("Defaults: q4f16, 512, \"This is a subsystem synthesis smoke test.\"."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunSubsystemSynthTest));
}
