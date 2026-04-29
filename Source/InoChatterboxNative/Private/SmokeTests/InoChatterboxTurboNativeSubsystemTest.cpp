// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.Chatterbox.SubsystemSynthTest
// ============================================================================
//
// Phase D Commit 3 smoke test: exercises UInoChatterboxTurboNativeSubsystem's
// full async API end-to-end through PIE (subsystem is a
// UGameInstanceSubsystem, so no active game = no subsystem = test
// bails with a clear error).
//
// Flow:
//   1. Grab the subsystem from the current GameInstance.
//   2. Build FInoChatterboxTurboNativeModelConfig{Variant = Q4F16 (or user arg)}.
//   3. Resolve default_voice.wav inside the staged variant dir — the
//      same file the existing native-layer Ino.Chatterbox.SynthTest
//      uses. Produced by setup-chatterbox.ps1 -IncludeDefaultVoice.
//   4. Call LoadModelsAsync with a dynamic-delegate callback.
//   5. OnLoaded success → build FInoChatterboxTurboNativeVoice{WavFilePath=...} +
//      FInoChatterboxTurboNativeSynthesisOptions, call SynthesizeAsync with a
//      second dynamic-delegate callback.
//   6. OnSynthComplete success → run waveform sanity checks (NaN /
//      inf / range), save to <Saved>/Chatterbox/subsystem_synth_test.wav
//      via UInoAudioFunctionLibrary::WriteInt16PcmBytesAsWav, log per-stage timings.
//   7. UnloadModels + RemoveFromRoot so the observer + its held
//      references can be collected.
//
// Non-blocking: LoadModelsAsync and SynthesizeAsync both return
// immediately; the editor stays responsive during the ~1-5 s load
// and the ~1-10 s synthesis.
//
// The existing native-layer Ino.Chatterbox.SynthTest stays — it
// exercises FInoChatterboxTurboNativeRunner directly without touching the
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

#include "InoChatterboxTurboNativeSubsystemTest.h"

#include "Audio/InoAudioFunctionLibrary.h"
#include "InoAgentsLog.h"
#include "InoSmokeTestCommon.h"
#include "ChatterboxTurboNative/InoChatterboxTurboNativeSubsystem.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

// ----------------------------------------------------------------------------
// OnLoaded handler
// ----------------------------------------------------------------------------

void UInoChatterboxTurboNativeSubsystemTestObserver::HandleLoaded(
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

    FInoChatterboxTurboNativeVoice Voice;
    Voice.WavFilePath = VoiceWavPath;

    FInoChatterboxTurboNativeSynthesisOptions Options;
    Options.MaxNewTokens      = MaxNewTokens;
    Options.RepetitionPenalty = 1.2f;

    FOnInoChatterboxTurboNativeSynthesisComplete OnComplete;
    OnComplete.BindDynamic(this, &UInoChatterboxTurboNativeSubsystemTestObserver::HandleSynthComplete);

    UE_LOG(LogInoAgents, Log,
           TEXT("SubsystemSynthTest: SynthesizeAsync dispatching — voice=%s, text=\"%s\", max=%d"),
           *VoiceWavPath, *PromptText, MaxNewTokens);

    Subsystem->SynthesizeAsync(PromptText, Voice, Options, OnComplete);
}

// ----------------------------------------------------------------------------
// OnSynthComplete handler
// ----------------------------------------------------------------------------

void UInoChatterboxTurboNativeSubsystemTestObserver::HandleSynthComplete(
    bool bSuccess,
    FInoChatterboxTurboNativeSynthesisResult Result,
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
        //
        // Result.AudioSamples is int16 PCM LE bytes now (Phase D
        // byte-based API). Int16 can't store NaN/Inf, and its values
        // are already bounded to [-32768, +32767], so the only checks
        // worth running are "non-empty" and "byte count is even".
        // Report min/max int16 values for diagnostic interest.
        const int32 NumBytes   = Result.AudioSamples.Num();
        const int32 NumSamples = NumBytes / 2;
        const bool  bAligned   = (NumBytes & 1) == 0;

        int16 MinS =  INT16_MAX;
        int16 MaxS =  INT16_MIN;
        if (bAligned && NumSamples > 0)
        {
            const int16* Src =
                reinterpret_cast<const int16*>(Result.AudioSamples.GetData());
            for (int32 i = 0; i < NumSamples; ++i)
            {
                if (Src[i] < MinS) MinS = Src[i];
                if (Src[i] > MaxS) MaxS = Src[i];
            }
        }

        const int32 SampleRate = Subsystem ? Subsystem->GetOutputSampleRate() : 24000;
        UE_LOG(LogInoAgents, Log,
               TEXT("SubsystemSynthTest: SynthesizeAsync SUCCESS in %.1f ms. ")
               TEXT("Bytes=%d (%d samples, %.2f s @ %d Hz), tokens=%d, stop=%s. ")
               TEXT("aligned=%s int16 min=%d max=%d."),
               ElapsedMs, NumBytes, NumSamples, Result.DurationSeconds,
               SampleRate, Result.NumGeneratedTokens,
               Result.bHitStopToken ? TEXT("yes") : TEXT("no"),
               bAligned ? TEXT("yes") : TEXT("NO"),
               (int32)MinS, (int32)MaxS);

        UE_LOG(LogInoAgents, Log,
               TEXT("SubsystemSynthTest: per-stage: encoder=%.1f ms embed=%.1f ms LM=%.1f ms decoder=%.1f ms total=%.1f ms"),
               Result.EncoderMs, Result.EmbedTotalMs, Result.LanguageModelMs,
               Result.DecoderMs, Result.TotalElapsedMs);

        // -------- Save WAV --------
        // Audio is already int16 PCM bytes — use the byte-oriented WAV
        // writer directly, no re-quantization.
        if (!OutputWavPath.IsEmpty() && NumSamples > 0)
        {
            const FString OutputDir = FPaths::GetPath(OutputWavPath);
            if (!OutputDir.IsEmpty())
            {
                IFileManager::Get().MakeDirectory(*OutputDir, /*Tree=*/ true);
            }

            const bool bWrote = UInoAudioFunctionLibrary::WriteInt16PcmBytesAsWav(
                OutputWavPath,
                MakeArrayView(Result.AudioSamples),
                Subsystem ? Subsystem->GetOutputSampleRate() : 24000);

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

        // PASS criteria: non-empty + byte-aligned. Out-of-range is
        // impossible for int16; NaN/Inf are impossible for int16; so
        // the only real failures are the ones above.
        const bool bWaveformOk = (NumSamples > 0) && bAligned;
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

        EInoChatterboxTurboNativeVariant VariantEnum;
        if (!ChatterboxVariantFromString(VariantStr, VariantEnum))
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("SubsystemSynthTest: unknown variant '%s' ")
                   TEXT("(expected q4f16 / fp16 / q4 / fp32 / quantized)"),
                   *VariantStr);
            return;
        }

        // -------- Subsystem lookup --------
        UInoChatterboxTurboNativeSubsystem* Subsys = InoSmokeTest::FindGameInstanceSubsystem<UInoChatterboxTurboNativeSubsystem>();
        if (Subsys == nullptr)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("SubsystemSynthTest: could not find UInoChatterboxTurboNativeSubsystem. ")
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
        // anonymous namespace — the pre-existing InoChatterboxTurboNativeTest.cpp
        // already has its own file-local ResolveChatterboxDir, and UBT's
        // unity build merges the two files' anonymous namespaces into one
        // TU, so a duplicated definition here collides at link time.
        const FString VariantDir  = ChatterboxResolveVariantDir(VariantEnum);
        const FString VoiceWav    = FPaths::Combine(VariantDir, TEXT("default_voice.wav"));
        const FString OutputWav   = FPaths::Combine(
            FPaths::ProjectSavedDir(), TEXT("Chatterbox"),
            TEXT("subsystem_synth_test.wav"));

        // -------- Observer --------
        auto* Observer = NewObject<UInoChatterboxTurboNativeSubsystemTestObserver>();
        Observer->StartTime     = FPlatformTime::Seconds();
        Observer->Subsystem     = Subsys;
        Observer->VoiceWavPath  = VoiceWav;
        Observer->PromptText    = PromptText;
        Observer->OutputWavPath = OutputWav;
        Observer->MaxNewTokens  = MaxNewTokens;
        Observer->AddToRoot();

        FInoChatterboxTurboNativeModelConfig Config;
        Config.Variant = VariantEnum;

        FOnInoChatterboxTurboNativeModelsLoaded OnLoaded;
        OnLoaded.BindDynamic(Observer, &UInoChatterboxTurboNativeSubsystemTestObserver::HandleLoaded);

        UE_LOG(LogInoAgents, Log,
               TEXT("SubsystemSynthTest: starting. variant=%s max_new_tokens=%d prompt=\"%s\""),
               *VariantStr, MaxNewTokens, *PromptText);
        UE_LOG(LogInoAgents, Log,
               TEXT("SubsystemSynthTest: LoadModelsAsync dispatching (non-blocking)."));

        // This smoke test doesn't observe download progress — pass an
        // unbound delegate so ExecuteIfBound no-ops the progress ticks.
        Subsys->LoadModelsAsync(Config, FOnInoChatterboxTurboNativeDownloadProgress(), OnLoaded);
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
