// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// InoAgents.ElevenLabs.DialogueStreamTest (phase 1 live smoke test)
// ============================================================================
//
// Live end-to-end test of the ElevenLabs Text-to-Dialogue stream endpoint
// through the UE-facing API surface:
//
//   - UElevenLabsSettings.ApiKey is read by the subsystem at PIE start.
//   - UElevenLabsTextToDialogueStream::StreamTextToDialogue dispatches
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
//      InoAgents.ElevenLabs.ReloadSettings.
//   2. Enter PIE.
//
// Invoke:
//      InoAgents.ElevenLabs.DialogueStreamTest
//
// Requires a valid API key with access to the two example voice IDs
// below. Free-tier keys may fail with HTTP 401/402 depending on the
// voices' tier requirements.
// ============================================================================

#include "InoAgentsElevenLabsDialogueStreamTest.h"

#include "ElevenLabs/ElevenLabsSubsystem.h"
#include "ElevenLabs/ElevenLabsTextToDialogueStream.h"
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
    constexpr const TCHAR* kVoiceA = TEXT("JBFqnCBsd6RMkjVDRZzb");
    constexpr const TCHAR* kVoiceB = TEXT("Aw4FAjKCGjjNkVhN1Xmq");

    UElevenLabsSubsystem* FindSubsystem()
    {
        if (GEngine == nullptr)
        {
            return nullptr;
        }
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (UGameInstance* GI = Context.OwningGameInstance)
            {
                if (UElevenLabsSubsystem* Subsys = GI->GetSubsystem<UElevenLabsSubsystem>())
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

        // Prefer a PIE world — that's where UElevenLabsSubsystem actually
        // lives. The editor preview world has no GameInstance, so handing
        // it to the async action's Activate() makes
        // UGameplayStatics::GetGameInstance return null and the subsystem
        // lookup fails with "No UElevenLabsSubsystem — call from a live
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
    const TCHAR* OutputFormatToExtension(EElevenLabsOutputFormat Fmt)
    {
        switch (Fmt)
        {
            case EElevenLabsOutputFormat::Mp3_44100_128:
            case EElevenLabsOutputFormat::Mp3_44100_64:
            case EElevenLabsOutputFormat::Mp3_22050_32:
                return TEXT(".mp3");
            case EElevenLabsOutputFormat::Pcm_16000:
            case EElevenLabsOutputFormat::Pcm_24000:
            case EElevenLabsOutputFormat::Pcm_44100:
                return TEXT(".pcm");
            case EElevenLabsOutputFormat::Ulaw_8000:
                return TEXT(".ulaw");
            default:
                return TEXT(".bin");
        }
    }
}

void UInoAgentsElevenLabsDialogueStreamTestObserver::HandleAudioChunk(
    const TArray<uint8>& AudioBytes, int64 TotalBytesReceived)
{
    NumChunks += 1;
    TotalBytes = TotalBytesReceived;

    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogInoAgents, Log,
           TEXT("DialogueStreamTest: chunk %3d (+%.3f s) — %d bytes (total %lld)"),
           NumChunks, Elapsed, AudioBytes.Num(), TotalBytesReceived);
}

void UInoAgentsElevenLabsDialogueStreamTestObserver::HandleComplete(
    const TArray<uint8>& FullAudioBytes, EElevenLabsOutputFormat Format)
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

void UInoAgentsElevenLabsDialogueStreamTestObserver::HandleError(FString ErrorMessage)
{
    const double Elapsed = FPlatformTime::Seconds() - StartTime;
    UE_LOG(LogInoAgents, Error,
           TEXT("DialogueStreamTest: FAILED after %.2f s (%d chunks, %lld bytes received): %s"),
           Elapsed, NumChunks, TotalBytes, *ErrorMessage);

    Finish();
}

void UInoAgentsElevenLabsDialogueStreamTestObserver::Finish()
{
    // The action already unregistered itself from the subsystem and
    // SetReadyToDestroy()'d itself inside FinishCleanly(). We just drop
    // our own reference and unroot.
    Action = nullptr;

    RemoveFromRoot();

    UE_LOG(LogInoAgents, Log, TEXT("DialogueStreamTest: DONE"));
}

static void RunElevenLabsDialogueStreamTest(const TArray<FString>& /*Args*/)
{
    UElevenLabsSubsystem* Subsys = FindSubsystem();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("DialogueStreamTest: no UElevenLabsSubsystem found — start PIE first."));
        return;
    }

    if (Subsys->GetApiKey().IsEmpty())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("DialogueStreamTest: API key is empty. Set it under "
                    "Project Settings -> Plugins -> InoAgents ElevenLabs, then "
                    "run InoAgents.ElevenLabs.ReloadSettings (or restart PIE)."));
        return;
    }

    UObject* Ctx = FindWorldContext();
    if (Ctx == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("DialogueStreamTest: no world context available."));
        return;
    }

    // Build a fixed two-line dialogue using the voice IDs from the
    // ElevenLabs docs example.
    FElevenLabsDialogueRequest Req;
    Req.Inputs.Add({ TEXT("Knock knock."),          FString(kVoiceA) });
    Req.Inputs.Add({ TEXT("Who's there?"),          FString(kVoiceB) });
    Req.Inputs.Add({ TEXT("A plugin, streaming."),  FString(kVoiceA) });
    Req.OutputFormat = Subsys->GetDefaultOutputFormat();

    UInoAgentsElevenLabsDialogueStreamTestObserver* Observer =
        NewObject<UInoAgentsElevenLabsDialogueStreamTestObserver>();
    Observer->StartTime    = FPlatformTime::Seconds();
    Observer->OutputFormat = Req.OutputFormat;
    Observer->AddToRoot();

    // Create the async action; bind delegates BEFORE calling Activate
    // so we don't miss the first chunk (which can fire synchronously on
    // some HTTP backends if the request is very small).
    UElevenLabsTextToDialogueStream* Action =
        UElevenLabsTextToDialogueStream::StreamTextToDialogue(Ctx, Req, FString());
    if (Action == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("DialogueStreamTest: StreamTextToDialogue returned null"));
        Observer->RemoveFromRoot();
        return;
    }
    Observer->Action = Action;

    Action->OnAudioChunk.AddDynamic(
        Observer, &UInoAgentsElevenLabsDialogueStreamTestObserver::HandleAudioChunk);
    Action->OnComplete.AddDynamic(
        Observer, &UInoAgentsElevenLabsDialogueStreamTestObserver::HandleComplete);
    Action->OnError.AddDynamic(
        Observer, &UInoAgentsElevenLabsDialogueStreamTestObserver::HandleError);

    UE_LOG(LogInoAgents, Log,
           TEXT("DialogueStreamTest: dispatching dialogue (%d lines)..."),
           Req.Inputs.Num());

    Action->Activate();
}

static FAutoConsoleCommand GElevenLabsDialogueStreamTestCommand(
    TEXT("InoAgents.ElevenLabs.DialogueStreamTest"),
    TEXT("Phase 1 ElevenLabs smoke test: POSTs a fixed 3-line dialogue to "
         "/v1/text-to-dialogue/stream, logs chunk sizes as they arrive, "
         "saves the result to Saved/InoAgents/ElevenLabs/test.<ext>, and "
         "logs PASS/FAIL. Requires a valid API key in Project Settings -> "
         "Plugins -> InoAgents ElevenLabs."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunElevenLabsDialogueStreamTest));

static void RunElevenLabsReloadSettings(const TArray<FString>& /*Args*/)
{
    UElevenLabsSubsystem* Subsys = FindSubsystem();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ReloadSettings: no UElevenLabsSubsystem found — start PIE first."));
        return;
    }
    Subsys->ReloadSettings();
}

static FAutoConsoleCommand GElevenLabsReloadSettingsCommand(
    TEXT("InoAgents.ElevenLabs.ReloadSettings"),
    TEXT("Re-read UElevenLabsSettings into the subsystem's cached fields "
         "without restarting PIE. Use after changing the API key or base "
         "URL in Project Settings -> Plugins -> InoAgents ElevenLabs."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunElevenLabsReloadSettings));
