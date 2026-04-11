// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "ElevenLabs/ElevenLabsSubsystem.h"

#include "ElevenLabs/ElevenLabsSettings.h"
#include "ElevenLabs/ElevenLabsTextToDialogueStream.h"
#include "InoAgentsLog.h"

#include "Kismet/BlueprintAsyncActionBase.h"

void UElevenLabsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    ReloadSettings();

    UE_LOG(LogInoAgents, Log,
           TEXT("UElevenLabsSubsystem: initialised (ApiKey=%s, BaseUrl=%s, DefaultModelId=%s)"),
           CachedApiKey.IsEmpty() ? TEXT("<empty>") : TEXT("<set>"),
           *CachedBaseUrl,
           *CachedDefaultModelId);
}

void UElevenLabsSubsystem::Deinitialize()
{
    // Kill any in-flight HTTP requests before the game instance tears
    // down. Without this, an HTTP response arriving after PIE end would
    // dispatch into a freed UBlueprintAsyncActionBase and crash in the
    // delegate broadcast.
    CancelAll();

    Super::Deinitialize();
}

void UElevenLabsSubsystem::ReloadSettings()
{
    const UElevenLabsSettings* Settings = UElevenLabsSettings::Get();
    if (Settings == nullptr)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("UElevenLabsSubsystem::ReloadSettings: UElevenLabsSettings::Get() "
                    "returned null; using built-in defaults"));
        CachedApiKey.Empty();
        CachedBaseUrl               = TEXT("https://api.elevenlabs.io");
        CachedDefaultModelId        = TEXT("eleven_v3");
        CachedDefaultOutputFormat   = EElevenLabsOutputFormat::Mp3_44100_128;
        return;
    }

    CachedApiKey                = Settings->ApiKey;
    CachedBaseUrl               = Settings->GetEffectiveBaseUrl();
    CachedDefaultModelId        = Settings->DefaultModelId;
    CachedDefaultOutputFormat   = Settings->DefaultOutputFormat;

    UE_LOG(LogInoAgents, Log,
           TEXT("UElevenLabsSubsystem::ReloadSettings: ApiKey=%s, BaseUrl=%s"),
           CachedApiKey.IsEmpty() ? TEXT("<empty>") : TEXT("<set>"),
           *CachedBaseUrl);
}

void UElevenLabsSubsystem::CancelAll()
{
    if (LiveActions.Num() == 0)
    {
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UElevenLabsSubsystem::CancelAll: cancelling %d live action(s)"),
           LiveActions.Num());

    // Copy into a local array before iterating — CancelStream() dispatches
    // the action's own Finish path, which calls UnregisterLiveAction back
    // on us and mutates LiveActions. Iterating the TSet directly while it
    // mutates would invalidate the iterator.
    TArray<UBlueprintAsyncActionBase*> Snapshot;
    Snapshot.Reserve(LiveActions.Num());
    for (const TObjectPtr<UBlueprintAsyncActionBase>& Action : LiveActions)
    {
        if (Action.Get() != nullptr)
        {
            Snapshot.Add(Action.Get());
        }
    }

    for (UBlueprintAsyncActionBase* Action : Snapshot)
    {
        // Phase 1 only has one async action type, so the dispatch is a
        // single cast. Phases 2 and 3 will add one branch each.
        if (UElevenLabsTextToDialogueStream* Stream =
                Cast<UElevenLabsTextToDialogueStream>(Action))
        {
            Stream->CancelStream();
            continue;
        }

        UE_LOG(LogInoAgents, Warning,
               TEXT("UElevenLabsSubsystem::CancelAll: unknown live action type %s; "
                    "dropping reference without cancel"),
               *Action->GetClass()->GetName());
    }

    // Anything that didn't cleanly unregister itself above (shouldn't
    // happen in practice) is force-dropped now so GC can collect it.
    LiveActions.Reset();
}

void UElevenLabsSubsystem::RegisterLiveAction(UBlueprintAsyncActionBase* Action)
{
    if (Action == nullptr)
    {
        return;
    }
    LiveActions.Add(Action);
}

void UElevenLabsSubsystem::UnregisterLiveAction(UBlueprintAsyncActionBase* Action)
{
    if (Action == nullptr)
    {
        return;
    }
    LiveActions.Remove(Action);
}
