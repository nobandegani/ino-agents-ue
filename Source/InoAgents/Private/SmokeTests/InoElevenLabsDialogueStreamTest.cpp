// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.ElevenLabs.DialogueStreamTest (phase 1 live smoke test)
// ============================================================================
//
// Live end-to-end test of the ElevenLabs Text-to-Dialogue stream endpoint
// through the UE-facing API surface:
//
//   - UElevenLabsSettings.ApiKey is read by the subsystem at PIE start.
//   - UInoElevenLabsTextToDialogueStream::StreamTextToDialogue dispatches
//     an HTTP POST to api.elevenlabs.io.
//   - OnAudioChunk fires as bytes arrive (true chunked streaming path
//     via UE HTTP's progress delegate).
//   - OnComplete delivers the full audio buffer; we save it to disk at
//     Saved/InoAgents/ElevenLabs/test.{mp3|pcm|ulaw} for playback.
//   - OnError catches missing-key / HTTP 4xx / network failures.
//
// Prereqs:
//   1. Set UElevenLabsSettings.ApiKey in Project Settings -> Plugins ->
//      InoAgents ElevenLabs. Restart PIE OR run
//      Ino.ElevenLabs.ReloadSettings.
//   2. Enter PIE.
//
// Invoke:
//      Ino.ElevenLabs.DialogueStreamTest
//
// Requires a valid API key with access to the two example voice IDs
// below. Free-tier keys may fail with HTTP 401/402 depending on the
// voices' tier requirements.
// ============================================================================

#include "InoElevenLabsDialogueStreamTest.h"

#include "ElevenLabs/InoElevenLabsSubsystem.h"
#include "ElevenLabs/InoElevenLabsTextToDialogueStream.h"
#include "InoAgentsLog.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
    // Defaults used when the console command is invoked with no args.
    //
    // Voice IDs in ElevenLabs are account-specific: public-library
    // voices return 404 unless they've been imported into the caller's
    // personal library. The two below are known-good IDs from the
    // project owner's ElevenLabs account — they won't work on every
    // developer's machine.
    //
    // If you hit voice_not_found, pass your own voice IDs as args:
    //   Ino.ElevenLabs.DialogueStreamTest <voiceA> <voiceB>
    // Find yours at https://elevenlabs.io/app/voice-lab or by calling
    //   GET https://api.elevenlabs.io/v1/voices
    // with your API key. The response's "voices[*].voice_id" fields
    // are the strings you want.
    constexpr const TCHAR* kDefaultVoiceA = TEXT("AyCt0WmAXUcPJR11zeeP");
    constexpr const TCHAR* kDefaultVoiceB = TEXT("lhgliD0TncfFOY1Nc93M");

    UInoElevenLabsSubsystem* FindSubsystem()
    {
        if (GEngine == nullptr)
        {
            return nullptr;
        }
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (UGameInstance* GI = Context.OwningGameInstance)
            {
                if (UInoElevenLabsSubsystem* Subsys = GI->GetSubsystem<UInoElevenLabsSubsystem>())
                {
                    return Subsys;
                }
            }
        }
        return nullptr;
    }

    UObject* FindWorldContext()
    {
        if (GEngine == nullptr)
        {
            return nullptr;
        }

        // Prefer a PIE world — that's where UInoElevenLabsSubsystem actually
        // lives. The editor preview world has no GameInstance, so handing
        // it to the async action's Activate() makes
        // UGameplayStatics::GetGameInstance return null and the subsystem
        // lookup fails with "No UInoElevenLabsSubsystem — call from a live
        // game instance". First successful PIE world wins.
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.WorldType == EWorldType::PIE && Context.World() != nullptr)
            {
                return Context.World();
            }
        }

        // Fallback: any world that has a game instance attached. Still
        // good enough for a subsystem lookup even if it's not strictly
        // PIE (e.g. standalone game).
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.OwningGameInstance != nullptr && Context.World() != nullptr)
            {
                return Context.World();
            }
        }

        return nullptr;
    }

    /** File extension (including dot) appropriate for a given output format. */
    const TCHAR* OutputFormatToExtension(EInoElevenLabsOutputFormat Fmt)
    {
        switch (Fmt)
        {
            case EInoElevenLabsOutputFormat::Mp3_44100_128:
            case EInoElevenLabsOutputFormat::Mp3_44100_64:
            case EInoElevenLabsOutputFormat::Mp3_22050_32:
                return TEXT(".mp3");
            case EInoElevenLabsOutputFormat::Pcm_16000:
            case EInoElevenLabsOutputFormat::Pcm_24000:
            case EInoElevenLabsOutputFormat::Pcm_44100:
                return TEXT(".pcm");
            case EInoElevenLabsOutputFormat::Ulaw_8000:
                return TEXT(".ulaw");
            default:
                return TEXT(".bin");
        }
    }
}

void UInoElevenLabsDialogueStreamTestObserver::HandleAudioChunk(
    const TArray<uint8>& AudioBytes, int64 TotalBytesReceived)
{
    NumChunks += 1;
    TotalBytes = TotalBytesReceived;

    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogInoAgents, Log,
           TEXT("DialogueStreamTest: chunk %3d (+%.3f s) — %d bytes (total %lld)"),
           NumChunks, Elapsed, AudioBytes.Num(), TotalBytesReceived);
}

void UInoElevenLabsDialogueStreamTestObserver::HandleComplete(
    const TArray<uint8>& FullAudioBytes, EInoElevenLabsOutputFormat Format)
{
    const double Total = FPlatformTime::Seconds() - StartTime;

    UE_LOG(LogInoAgents, Log,
           TEXT("DialogueStreamTest: COMPLETE — %d chunks, %d bytes, %.2f s"),
           NumChunks, FullAudioBytes.Num(), Total);

    // Build the output path and ensure the directory exists.
    const FString Dir = FPaths::ProjectSavedDir() / TEXT("InoAgents/ElevenLabs");
    IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);

    OutputFilePath = Dir / (FString(TEXT("test")) + OutputFormatToExtension(Format));
    OutputFilePath = FPaths::ConvertRelativePathToFull(OutputFilePath);

    if (FFileHelper::SaveArrayToFile(FullAudioBytes, *OutputFilePath))
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("DialogueStreamTest: PASS — saved to %s"),
               *OutputFilePath);
    }
    else
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("DialogueStreamTest: FAIL — SaveArrayToFile failed for %s"),
               *OutputFilePath);
    }

    Finish();
}

void UInoElevenLabsDialogueStreamTestObserver::HandleError(FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogInoAgents, Error,
           TEXT("DialogueStreamTest: FAILED after %.2f s (%d chunks, %lld bytes received): %s"),
           Elapsed, NumChunks, TotalBytes, *ErrorMessage);

    Finish();
}

void UInoElevenLabsDialogueStreamTestObserver::Finish()
{
    // The action already unregistered itself from the subsystem and
    // SetReadyToDestroy()'d itself inside FinishCleanly(). We just drop
    // our own reference and unroot.
    Action = nullptr;

    RemoveFromRoot();

    UE_LOG(LogInoAgents, Log, TEXT("DialogueStreamTest: DONE"));
}

static void RunElevenLabsDialogueStreamTest(const TArray<FString>& Args)
{
    UInoElevenLabsSubsystem* Subsys = FindSubsystem();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("DialogueStreamTest: no UInoElevenLabsSubsystem found — start PIE first."));
        return;
    }

    if (Subsys->GetApiKey().IsEmpty())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("DialogueStreamTest: API key is empty. Set it under "
                    "Project Settings -> Plugins -> InoAgents ElevenLabs, then "
                    "run Ino.ElevenLabs.ReloadSettings (or restart PIE)."));
        return;
    }

    UObject* Ctx = FindWorldContext();
    if (Ctx == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("DialogueStreamTest: no world context available."));
        return;
    }

    // Optional voice ID overrides from console args:
    //   Ino.ElevenLabs.DialogueStreamTest            -> defaults for both
    //   Ino.ElevenLabs.DialogueStreamTest VA         -> VA for A, default for B
    //   Ino.ElevenLabs.DialogueStreamTest VA VB      -> custom for both
    const FString VoiceA = (Args.Num() >= 1) ? Args[0] : FString(kDefaultVoiceA);
    const FString VoiceB = (Args.Num() >= 2) ? Args[1] : FString(kDefaultVoiceB);

    UE_LOG(LogInoAgents, Log,
           TEXT("DialogueStreamTest: voiceA=%s, voiceB=%s "
                "(override with: Ino.ElevenLabs.DialogueStreamTest <voiceA> <voiceB>)"),
           *VoiceA, *VoiceB);

    // Build a three-line dialogue that alternates between the two voices.
    FInoElevenLabsDialogueRequest Req;
    Req.Inputs.Add({ TEXT("Knock knock."),          VoiceA });
    Req.Inputs.Add({ TEXT("Who's there?"),          VoiceB });
    Req.Inputs.Add({ TEXT("A plugin, streaming."),  VoiceA });
    Req.OutputFormat = Subsys->GetDefaultOutputFormat();

    UInoElevenLabsDialogueStreamTestObserver* Observer =
        NewObject<UInoElevenLabsDialogueStreamTestObserver>();
    Observer->StartTime    = FPlatformTime::Seconds();
    Observer->OutputFormat = Req.OutputFormat;
    Observer->AddToRoot();

    // Create the async action; bind delegates BEFORE calling Activate
    // so we don't miss the first chunk (which can fire synchronously on
    // some HTTP backends if the request is very small).
    UInoElevenLabsTextToDialogueStream* Action =
        UInoElevenLabsTextToDialogueStream::StreamTextToDialogue(Ctx, Req, FString());
    if (Action == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("DialogueStreamTest: StreamTextToDialogue returned null"));
        Observer->RemoveFromRoot();
        return;
    }
    Observer->Action = Action;

    Action->OnAudioChunk.AddDynamic(
        Observer, &UInoElevenLabsDialogueStreamTestObserver::HandleAudioChunk);
    Action->OnComplete.AddDynamic(
        Observer, &UInoElevenLabsDialogueStreamTestObserver::HandleComplete);
    Action->OnError.AddDynamic(
        Observer, &UInoElevenLabsDialogueStreamTestObserver::HandleError);

    UE_LOG(LogInoAgents, Log,
           TEXT("DialogueStreamTest: dispatching dialogue (%d lines)..."),
           Req.Inputs.Num());

    Action->Activate();
}

static FAutoConsoleCommand GElevenLabsDialogueStreamTestCommand(
    TEXT("Ino.ElevenLabs.DialogueStreamTest"),
    TEXT("Phase 1 ElevenLabs smoke test: POSTs a 3-line dialogue to "
         "/v1/text-to-dialogue/stream, logs chunk sizes as they arrive, "
         "saves the result to Saved/InoAgents/ElevenLabs/test.<ext>, and "
         "logs PASS/FAIL. Requires a valid API key in Project Settings -> "
         "Plugins -> InoAgents ElevenLabs. Optional voice IDs from your "
         "own library: Ino.ElevenLabs.DialogueStreamTest <voiceA> <voiceB>. "
         "Get your voice IDs at https://elevenlabs.io/app/voice-lab or "
         "via GET /v1/voices."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunElevenLabsDialogueStreamTest));

static void RunElevenLabsReloadSettings(const TArray<FString>& /*Args*/)
{
    UInoElevenLabsSubsystem* Subsys = FindSubsystem();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ReloadSettings: no UInoElevenLabsSubsystem found — start PIE first."));
        return;
    }
    Subsys->ReloadSettings();
}

static FAutoConsoleCommand GElevenLabsReloadSettingsCommand(
    TEXT("Ino.ElevenLabs.ReloadSettings"),
    TEXT("Re-read UElevenLabsSettings into the subsystem's cached fields "
         "without restarting PIE. Use after changing the API key or base "
         "URL in Project Settings -> Plugins -> InoAgents ElevenLabs."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunElevenLabsReloadSettings));
