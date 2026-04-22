// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.NeuTtsNano.SynthTest (Milestone 5 verification)
// ============================================================================
//
// Full end-to-end smoke for the NeuTTS Nano subsystem. Usable from the
// Output Log under PIE:
//
//     Ino.NeuTtsNano.SynthTest                # uses the default phoneme string
//     Ino.NeuTtsNano.SynthTest hɛloʊ wɝːld    # custom pre-phonemized IPA
//
// Flow:
//   1. Locate UInoNeuTtsNanoSubsystem (requires PIE — GameInstanceSubsystems
//      only exist once a game instance is up).
//   2. If IsModelLoaded()==false, call LoadModelAsync to stand up the
//      runtime (download if needed, ~1-3 min cold, ~1.7 s warm).
//   3. On load success, call SynthesizeAsync with the phonemes from args
//      (or the baked-in default "Hello my name is Andy").
//   4. On synth success, write Saved/InoNeuTtsNanoTest.wav via
//      UInoAudioFunctionLibrary::SaveInt16PcmAsWav and log:
//        - wall-clock time for the full synth call
//        - PCM size in samples / bytes / seconds of audio
//        - real-time factor (audio_seconds / synth_seconds)
//
// Every failure path is logged at Error level and the observer tears
// down cleanly via RemoveFromRoot — no leaked UObjects if synth errors.
// ============================================================================

#include "InoNeuTtsNanoSynthTest.h"

#include "Audio/InoAudioFunctionLibrary.h"
#include "InoAgentsLog.h"
#include "InoSmokeTestCommon.h"
#include "NeuTtsNano/InoNeuTtsNanoSubsystem.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

namespace
{
    /**
     * Default phonemes when the caller supplies no args. espeak-ng en-us
     * IPA for "Hello my name is Andy. It's nice to meet you." —
     * chosen short-ish so the smoke test runs in a few seconds while
     * still producing enough audio to hear whether the pipeline works.
     *
     * If you change this, regenerate with:
     *   echo "your text" | espeak-ng -x -q --ipa
     * or via the phonemizer Python library (same backend).
     */
    constexpr const TCHAR* kDefaultPhonemes =
        TEXT("hɛloʊ maɪ neɪm ɪz ændi. ɪts naɪs tə miːt juː.");
}

void UInoNeuTtsNanoSynthTestObserver::HandleLoaded(
    bool bSuccess, FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - LoadStartTime;

    if (!bSuccess)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("SynthTest: LoadModelAsync FAILED after %.2f s: %s"),
               Elapsed, *ErrorMessage);
        Finish();
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("SynthTest: model loaded in %.2f s — kicking off synthesis."),
           Elapsed);
    KickOffSynthesis();
}

void UInoNeuTtsNanoSynthTestObserver::KickOffSynthesis()
{
    if (!IsValid(Subsystem))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("SynthTest: Subsystem pointer went stale."));
        Finish();
        return;
    }

    SynthStartTime = FPlatformTime::Seconds();

    FOnInoNeuTtsNanoSynthesisComplete SynthDelegate;
    SynthDelegate.BindDynamic(this, &UInoNeuTtsNanoSynthTestObserver::HandleSynthComplete);

    UE_LOG(LogInoAgents, Log,
           TEXT("SynthTest: calling SynthesizeAsync "
                "(phonemes=\"%s\", voice=Default)"),
           *Phonemes);

    Subsystem->SynthesizeAsync(
        Phonemes,
        FName(TEXT("Default")),
        Options,
        SynthDelegate);
}

void UInoNeuTtsNanoSynthTestObserver::HandleSynthComplete(
    bool bSuccess,
    const TArray<uint8>& PcmInt16LE,
    int32 SampleRate,
    FString ErrorMessage)
{
    const double SynthElapsed = FPlatformTime::Seconds() - SynthStartTime;

    if (!bSuccess)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("SynthTest: SynthesizeAsync FAILED after %.2f s: %s"),
               SynthElapsed, *ErrorMessage);
        Finish();
        return;
    }

    const int64 ByteCount  = PcmInt16LE.Num();
    const int64 SampleCount = ByteCount / 2;          // int16 = 2 bytes
    const double AudioSec  = (double)SampleCount / FMath::Max(1, SampleRate);
    const double RtFactor  = AudioSec / FMath::Max(0.001, SynthElapsed);

    UE_LOG(LogInoAgents, Log,
           TEXT("SynthTest: synthesis OK — %lld samples (%.2f s audio @ %d Hz), "
                "synth wall-clock %.2f s (%.2fx real-time)."),
           SampleCount, AudioSec, SampleRate,
           SynthElapsed, RtFactor);

    // Write WAV.
    const bool bSaved =
        UInoAudioFunctionLibrary::SaveInt16PcmAsWav(OutputPath, PcmInt16LE, SampleRate);
    if (bSaved)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("SynthTest: WAV written — %s (%lld bytes total incl. header)"),
               *OutputPath, (int64)(ByteCount + 44));
    }
    else
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("SynthTest: SaveInt16PcmAsWav returned false for %s "
                    "(check directory exists + disk space)."),
               *OutputPath);
    }

    const double TotalElapsed = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogInoAgents, Log,
           TEXT("SynthTest: DONE in %.2f s total."), TotalElapsed);

    Finish();
}

void UInoNeuTtsNanoSynthTestObserver::Finish()
{
    RemoveFromRoot();
}

static void RunNeuTtsNanoSynthTest(const TArray<FString>& Args)
{
    UInoNeuTtsNanoSubsystem* Subsys = InoSmokeTest::FindNeuTtsNanoSubsystem();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("SynthTest: no UInoNeuTtsNanoSubsystem. Enter PIE first."));
        return;
    }

    // Compose phonemes string from all trailing args (joined by spaces).
    // This lets `Ino.NeuTtsNano.SynthTest h ɛ l oʊ` work even though UE
    // splits on whitespace.
    FString Phonemes;
    if (Args.Num() > 0)
    {
        Phonemes = FString::Join(Args, TEXT(" "));
    }
    else
    {
        Phonemes = kDefaultPhonemes;
    }

    // Output path under the game's Saved dir so it's easy to find and
    // doesn't pollute the plugin's Resources. Use a fixed filename so
    // repeated runs overwrite rather than accumulating.
    const FString OutputPath = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("InoNeuTtsNanoTest.wav"));

    auto* Observer = NewObject<UInoNeuTtsNanoSynthTestObserver>();
    Observer->StartTime  = FPlatformTime::Seconds();
    Observer->Subsystem  = Subsys;
    Observer->Phonemes   = Phonemes;
    Observer->OutputPath = OutputPath;
    Observer->Options    = FInoNeuTtsNanoSynthesisOptions{};   // defaults
    Observer->AddToRoot();

    // Two code paths based on current load state:
    //   - Not loaded -> async LoadModelAsync, chain into synth on success
    //   - Already loaded -> jump straight into synthesis
    if (Subsys->IsModelLoaded())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("SynthTest: model already loaded, skipping LoadModelAsync."));
        Observer->KickOffSynthesis();
    }
    else
    {
        Observer->LoadStartTime = FPlatformTime::Seconds();

        FOnInoNeuTtsNanoModelLoaded LoadDelegate;
        LoadDelegate.BindDynamic(
            Observer, &UInoNeuTtsNanoSynthTestObserver::HandleLoaded);

        FInoNeuTtsNanoModelConfig Config;
        Config.Variant = EInoNeuTtsNanoBackboneVariant::Q4;

        UE_LOG(LogInoAgents, Log,
               TEXT("SynthTest: model not loaded — calling LoadModelAsync first "
                    "(cold cache = ~1-3 min, warm cache = ~1.7 s)."));
        Subsys->LoadModelAsync(Config, LoadDelegate);
    }
}

static FAutoConsoleCommand GNeuTtsNanoSynthTestCmd(
    TEXT("Ino.NeuTtsNano.SynthTest"),
    TEXT("Milestone 5 smoke test: loads the NeuTTS Nano Q4 variant if not "
         "already loaded, runs SynthesizeAsync with the caller's phonemes "
         "(or a baked-in default), saves the int16 PCM to "
         "Saved/InoNeuTtsNanoTest.wav, and logs real-time factor. PIE required. "
         "Args: pre-phonemized IPA text (joined by spaces)."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunNeuTtsNanoSynthTest));
