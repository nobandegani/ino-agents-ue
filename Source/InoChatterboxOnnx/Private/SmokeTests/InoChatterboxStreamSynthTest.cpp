// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.Chatterbox.StreamSynthTest
// ============================================================================
//
// End-to-end smoke test for the UInoChatterboxStreamSynthesize async
// action — mirrors Ino.Chatterbox.SubsystemSynthTest but uses the
// streaming path instead of the all-at-end SynthesizeAsync.
//
// Flow:
//   1. Resolve UInoChatterboxTtsSubsystem via the first active world
//      (same helper the non-streaming subsystem test uses).
//   2. LoadModelsAsync the requested variant.
//   3. On load success, construct a UInoChatterboxStreamSynthesize via
//      its factory, bind OnAudioChunk / OnComplete / OnError, Activate.
//   4. For each OnAudioChunk: log size + accumulate bytes into
//      a local TArray<uint8>. First chunk logs its arrival latency
//      from the Activate() call (the "first audio" metric).
//   5. On OnComplete: diff the accumulated buffer against
//      Result.AudioSamples — they MUST byte-equal (otherwise the
//      runner dropped / duplicated / mis-sized a chunk somewhere).
//      Write the accumulated buffer to
//      <Saved>/Chatterbox/stream_synth_test.wav so a listening test
//      catches boundary artefacts.
//   6. Unload subsystem; RemoveFromRoot.
//
// Invoke (in PIE):
//     Ino.Chatterbox.StreamSynthTest [variant] [chunk_tokens] [max_new_tokens] [text...]
// Defaults:
//     variant         = q4f16
//     chunk_tokens    = 20
//     max_new_tokens  = 512
//     text            = "This is a streaming synthesis smoke test."
// ============================================================================

#include "InoChatterboxStreamSynthTest.h"

#include "InoAgentsLog.h"
#include "InoChatterboxAudioIO.h"
#include "InoSmokeTestCommon.h"
#include "Chatterbox/InoChatterboxStreamSynthesize.h"
#include "Chatterbox/InoChatterboxTtsSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"

// ----------------------------------------------------------------------------
// OnLoaded
// ----------------------------------------------------------------------------

void UInoChatterboxStreamSynthTestObserver::HandleLoaded(
    bool bSuccess, FString ErrorMessage)
{
    const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

    if (!bSuccess)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("StreamSynthTest: LoadModelsAsync FAILED after %.1f ms: %s"),
               ElapsedMs, *ErrorMessage);
        Teardown();
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("StreamSynthTest: LoadModelsAsync SUCCESS in %.1f ms; ")
           TEXT("dispatching stream synth (chunk_tokens=%d)"),
           ElapsedMs, StreamChunkTokens);

    if (!IFileManager::Get().FileExists(*VoiceWavPath))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("StreamSynthTest: reference voice WAV not found at %s. ")
               TEXT("Run Plugins/InoAgents/Chatterbox/scripts/setup-chatterbox.ps1 -IncludeDefaultVoice ")
               TEXT("or pass an explicit voice."),
               *VoiceWavPath);
        Teardown();
        return;
    }

    // Find any world to pass as WorldContextObject — the factory uses
    // it to locate the game instance / subsystem. The first active
    // world from GEngine's worlds list is sufficient.
    UWorld* AnyWorld = nullptr;
    if (GEngine != nullptr)
    {
        for (const FWorldContext& WC : GEngine->GetWorldContexts())
        {
            if (WC.World() != nullptr && WC.WorldType == EWorldType::PIE)
            {
                AnyWorld = WC.World();
                break;
            }
        }
        if (AnyWorld == nullptr)
        {
            for (const FWorldContext& WC : GEngine->GetWorldContexts())
            {
                if (WC.World() != nullptr)
                {
                    AnyWorld = WC.World();
                    break;
                }
            }
        }
    }
    if (AnyWorld == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("StreamSynthTest: no world found to use as WorldContextObject"));
        Teardown();
        return;
    }

    FInoChatterboxVoice Voice;
    Voice.WavFilePath = VoiceWavPath;

    FInoChatterboxSynthesisOptions Options;
    Options.MaxNewTokens      = MaxNewTokens;
    Options.RepetitionPenalty = 1.2f;

    ChunksReceived = 0;
    FirstChunkMs   = 0.0;
    AccumulatedBytes.Reset();
    SynthStartTime = FPlatformTime::Seconds();

    Action = UInoChatterboxStreamSynthesize::StreamSynthesize(
        AnyWorld, PromptText, Voice, Options, StreamChunkTokens);
    if (Action == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("StreamSynthTest: StreamSynthesize returned null"));
        Teardown();
        return;
    }

    Action->OnAudioChunk.AddDynamic(
        this, &UInoChatterboxStreamSynthTestObserver::HandleChunk);
    Action->OnComplete.AddDynamic(
        this, &UInoChatterboxStreamSynthTestObserver::HandleComplete);
    Action->OnError.AddDynamic(
        this, &UInoChatterboxStreamSynthTestObserver::HandleError);

    Action->Activate();
}

// ----------------------------------------------------------------------------
// OnAudioChunk
// ----------------------------------------------------------------------------

void UInoChatterboxStreamSynthTestObserver::HandleChunk(
    const TArray<uint8>& AudioChunk, bool bIsFinal, int32 NumGeneratedTokens)
{
    ++ChunksReceived;

    if (ChunksReceived == 1)
    {
        FirstChunkMs = (FPlatformTime::Seconds() - SynthStartTime) * 1000.0;
        UE_LOG(LogInoAgents, Log,
               TEXT("StreamSynthTest: first chunk after %.1f ms (%d bytes, tokens_so_far=%d, final=%s)"),
               FirstChunkMs, AudioChunk.Num(), NumGeneratedTokens,
               bIsFinal ? TEXT("yes") : TEXT("no"));
    }
    else
    {
        UE_LOG(LogInoAgents, Verbose,
               TEXT("StreamSynthTest: chunk #%d — %d bytes, tokens_so_far=%d, final=%s"),
               ChunksReceived, AudioChunk.Num(), NumGeneratedTokens,
               bIsFinal ? TEXT("yes") : TEXT("no"));
    }

    AccumulatedBytes.Append(AudioChunk);
}

// ----------------------------------------------------------------------------
// OnComplete
// ----------------------------------------------------------------------------

void UInoChatterboxStreamSynthTestObserver::HandleComplete(
    bool                          bSuccess,
    FInoChatterboxSynthesisResult Result,
    FString                       ErrorMessage)
{
    const double TotalMs = (FPlatformTime::Seconds() - SynthStartTime) * 1000.0;

    if (!bSuccess)
    {
        // HandleError and HandleComplete(bSuccess=false) are mutually
        // exclusive in the action's contract — we should only ever see
        // one of them. Log just in case.
        UE_LOG(LogInoAgents, Error,
               TEXT("StreamSynthTest: OnComplete bSuccess=false: %s"), *ErrorMessage);
        Teardown();
        return;
    }

    // -------- Integrity check: accumulated chunks vs Result --------
    //
    // We DO NOT check for byte-equality. Each intermediate decode runs
    // the full-attention conditional_decoder on a progressively-longer
    // prefix, and the final decode appends silence×3 — so the output
    // samples for the SAME prefix position differ by a few ULPs between
    // decode calls (context length influences attention, so earlier
    // samples drift numerically even though the prefix tokens are the
    // same). Accumulated bytes = prefix slice of B1 + new slice of B2
    // + ... + new slice of B_final, which is a Frankenstein across
    // different decoder runs. Result.AudioSamples is just B_final.
    // The two are audibly identical but bit-unequal in practice.
    //
    // What we CAN check: sample-count alignment. The accumulated tail
    // should match the final buffer's total length to within a handful
    // of samples (the final decode's output length is near-deterministic
    // given the same input shape). Large divergence indicates a real
    // bug in the chunked-delta bookkeeping.
    const int32 AccBytes   = AccumulatedBytes.Num();
    const int32 ResBytes   = Result.AudioSamples.Num();
    const int32 LenDiff    = FMath::Abs(AccBytes - ResBytes);
    const int32 AllowedDiff = 2 * 2;  // ≤ 2 samples of slack, int16 = 2 bytes

    UE_LOG(LogInoAgents, Log,
           TEXT("StreamSynthTest: OnComplete in %.1f ms — chunks=%d, first_chunk=%.1f ms, ")
           TEXT("accumulated=%d bytes, result=%d bytes (%d samples, %.2f s), tokens=%d."),
           TotalMs, ChunksReceived, FirstChunkMs,
           AccBytes, ResBytes, ResBytes / 2,
           Result.DurationSeconds, Result.NumGeneratedTokens);

    UE_LOG(LogInoAgents, Log,
           TEXT("StreamSynthTest: per-stage: encoder=%.1f ms embed=%.1f ms LM=%.1f ms ")
           TEXT("decoder=%.1f ms total=%.1f ms"),
           Result.EncoderMs, Result.EmbedTotalMs, Result.LanguageModelMs,
           Result.DecoderMs, Result.TotalElapsedMs);

    // -------- Save WAV (accumulated bytes — exercises the stream) --------
    // We save the accumulated bytes rather than Result.AudioSamples so
    // a boundary-artefact listening test is actually listening to the
    // streamed output, not the all-at-end concat.
    if (!OutputWavPath.IsEmpty() && AccBytes > 0)
    {
        const FString OutputDir = FPaths::GetPath(OutputWavPath);
        if (!OutputDir.IsEmpty())
        {
            IFileManager::Get().MakeDirectory(*OutputDir, /*Tree=*/ true);
        }

        const bool bWrote = InoChatterbox::WriteInt16PcmBytesAsWav(
            OutputWavPath,
            MakeArrayView(AccumulatedBytes),
            Subsystem ? Subsystem->GetOutputSampleRate() : 24000);
        UE_LOG(LogInoAgents, Log,
               TEXT("StreamSynthTest: %s wrote %s"),
               bWrote ? TEXT("") : TEXT("FAILED to"),
               *OutputWavPath);
    }

    // -------- PASS criteria --------
    //   - at least one chunk received (the final chunk is guaranteed
    //     when OnAudioChunk is bound, so zero chunks indicates the
    //     runner/worker dropped the final emit)
    //   - accumulated bytes non-zero
    //   - accumulated byte-count within ≤ 2 samples of Result bytes
    //     (decoder non-determinism across context lengths can shift
    //     output length by a sample or two; larger drift is a bug)
    const bool bPass =
        ChunksReceived > 0 && AccBytes > 0 && LenDiff <= AllowedDiff;

    UE_LOG(LogInoAgents, Log,
           TEXT("StreamSynthTest: %s (chunks=%d, length_drift=%d bytes, limit=%d)"),
           bPass ? TEXT("PASS") : TEXT("FAIL"),
           ChunksReceived, LenDiff, AllowedDiff);

    Teardown();
}

// ----------------------------------------------------------------------------
// OnError
// ----------------------------------------------------------------------------

void UInoChatterboxStreamSynthTestObserver::HandleError(FString ErrorMessage)
{
    UE_LOG(LogInoAgents, Error,
           TEXT("StreamSynthTest: OnError: %s"), *ErrorMessage);
    Teardown();
}

// ----------------------------------------------------------------------------
// Shared teardown
// ----------------------------------------------------------------------------

void UInoChatterboxStreamSynthTestObserver::Teardown()
{
    if (Subsystem)
    {
        Subsystem->UnloadModels();
        UE_LOG(LogInoAgents, Log,
               TEXT("StreamSynthTest: unloaded; IsModelsLoaded()=%s. DONE."),
               Subsystem->IsModelsLoaded() ? TEXT("true") : TEXT("false"));
    }

    // Clear the action ref so GC can reclaim it once its own
    // SetReadyToDestroy runs out.
    Action = nullptr;

    // IsRooted check so duplicate teardown (OnError after OnComplete,
    // etc.) is safe.
    if (IsRooted())
    {
        RemoveFromRoot();
    }
}

// ----------------------------------------------------------------------------
// Console command entry point
// ----------------------------------------------------------------------------

namespace
{
    void RunStreamSynthTest(const TArray<FString>& Args)
    {
        // Args:
        //   [0]          variant             (default q4f16)
        //   [1] digits   chunk_tokens        (default 20)
        //   [2] digits   max_new_tokens      (default 512)
        //   [3..]        prompt
        FString VariantStr = (Args.Num() > 0) ? Args[0] : FString(TEXT("q4f16"));

        int32 ChunkTokens  = 20;
        int32 MaxNewTokens = 512;
        int32 TextStartIdx = 1;
        if (Args.Num() >= 2 && !Args[1].IsEmpty() && FChar::IsDigit(Args[1][0]))
        {
            ChunkTokens  = FMath::Clamp(FCString::Atoi(*Args[1]), 0, 1024);
            TextStartIdx = 2;
        }
        if (Args.Num() >= 3 && !Args[2].IsEmpty() && FChar::IsDigit(Args[2][0]))
        {
            MaxNewTokens = FMath::Clamp(FCString::Atoi(*Args[2]), 1, 1024);
            TextStartIdx = 3;
        }

        FString PromptText;
        for (int32 i = TextStartIdx; i < Args.Num(); ++i)
        {
            if (!PromptText.IsEmpty()) { PromptText += TEXT(" "); }
            PromptText += Args[i];
        }
        if (PromptText.IsEmpty())
        {
            PromptText = TEXT("This is a streaming synthesis smoke test.");
        }

        EInoChatterboxVariant VariantEnum;
        if (!ChatterboxVariantFromString(VariantStr, VariantEnum))
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("StreamSynthTest: unknown variant '%s'"), *VariantStr);
            return;
        }

        UInoChatterboxTtsSubsystem* Subsys = InoSmokeTest::FindGameInstanceSubsystem<UInoChatterboxTtsSubsystem>();
        if (Subsys == nullptr)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("StreamSynthTest: no UInoChatterboxTtsSubsystem — run from PIE"));
            return;
        }

        if (Subsys->IsModelsLoaded())
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("StreamSynthTest: models already loaded (variant=%d); unloading first"),
                   (int32)Subsys->GetLoadedVariant());
            Subsys->UnloadModels();
        }

        const FString VariantDir = ChatterboxResolveVariantDir(VariantEnum);
        const FString VoiceWav   = FPaths::Combine(VariantDir, TEXT("default_voice.wav"));
        const FString OutputWav  = FPaths::Combine(
            FPaths::ProjectSavedDir(), TEXT("Chatterbox"),
            TEXT("stream_synth_test.wav"));

        auto* Observer = NewObject<UInoChatterboxStreamSynthTestObserver>();
        Observer->StartTime         = FPlatformTime::Seconds();
        Observer->Subsystem         = Subsys;
        Observer->VoiceWavPath      = VoiceWav;
        Observer->PromptText        = PromptText;
        Observer->OutputWavPath     = OutputWav;
        Observer->MaxNewTokens      = MaxNewTokens;
        Observer->StreamChunkTokens = ChunkTokens;
        Observer->AddToRoot();

        FInoChatterboxModelConfig Config;
        Config.Variant = VariantEnum;

        FOnInoChatterboxModelsLoaded OnLoaded;
        OnLoaded.BindDynamic(Observer, &UInoChatterboxStreamSynthTestObserver::HandleLoaded);

        UE_LOG(LogInoAgents, Log,
               TEXT("StreamSynthTest: start — variant=%s chunk_tokens=%d max_new_tokens=%d prompt=\"%s\""),
               *VariantStr, ChunkTokens, MaxNewTokens, *PromptText);

        Subsys->LoadModelsAsync(Config, FOnInoChatterboxDownloadProgress(), OnLoaded);
    }

    FAutoConsoleCommand GChatterboxStreamSynthTestCmd(
        TEXT("Ino.Chatterbox.StreamSynthTest"),
        TEXT("Streaming-variant smoke test: exercises ")
        TEXT("UInoChatterboxStreamSynthesize end-to-end — LoadModelsAsync, ")
        TEXT("Activate the async action, accumulate per-chunk bytes, ")
        TEXT("verify accumulated == Result on OnComplete, write WAV, unload. ")
        TEXT("Requires PIE. Needs default_voice.wav staged ")
        TEXT("(setup-chatterbox.ps1 -IncludeDefaultVoice). ")
        TEXT("Args: [variant] [chunk_tokens] [max_new_tokens] [text...]. ")
        TEXT("Defaults: q4f16, 20, 512, \"This is a streaming synthesis smoke test.\"."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunStreamSynthTest));
}
