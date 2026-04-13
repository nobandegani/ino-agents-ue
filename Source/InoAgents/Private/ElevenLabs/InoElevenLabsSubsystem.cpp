// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "ElevenLabs/InoElevenLabsSubsystem.h"

#include "ElevenLabs/InoElevenLabsTextToDialogueStream.h"
#include "InoAgentsLog.h"
#include "InoAgentsSettings.h"

#include "Kismet/BlueprintAsyncActionBase.h"

void UInoElevenLabsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    ReloadSettings();

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoElevenLabsSubsystem: initialised (ApiKey=%s, BaseUrl=%s, DefaultModelId=%s)"),
           CachedApiKey.IsEmpty() ? TEXT("<empty>") : TEXT("<set>"),
           *CachedBaseUrl,
           *CachedDefaultModelId);
}

void UInoElevenLabsSubsystem::Deinitialize()
{
    // Kill any in-flight HTTP requests before the game instance tears
    // down. Without this, an HTTP response arriving after PIE end would
    // dispatch into a freed UBlueprintAsyncActionBase and crash in the
    // delegate broadcast.
    CancelAll();

    Super::Deinitialize();
}

void UInoElevenLabsSubsystem::ReloadSettings()
{
    const UInoAgentsSettings* Settings = UInoAgentsSettings::Get();
    if (Settings == nullptr)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("UInoElevenLabsSubsystem::ReloadSettings: UInoAgentsSettings::Get() "
                    "returned null; using built-in defaults"));
        CachedApiKey.Empty();
        CachedBaseUrl               = TEXT("https://api.elevenlabs.io");
        CachedDefaultModelId        = TEXT("eleven_v3");
        CachedDefaultOutputFormat   = EInoElevenLabsOutputFormat::Mp3_44100_128;
        return;
    }

    CachedApiKey                = Settings->ElevenLabsApiKey;
    CachedBaseUrl               = Settings->GetEffectiveElevenLabsBaseUrl();
    CachedDefaultModelId        = Settings->ElevenLabsDefaultModelId;
    CachedDefaultOutputFormat   = Settings->ElevenLabsDefaultOutputFormat;

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoElevenLabsSubsystem::ReloadSettings: ApiKey=%s, BaseUrl=%s"),
           CachedApiKey.IsEmpty() ? TEXT("<empty>") : TEXT("<set>"),
           *CachedBaseUrl);
}

void UInoElevenLabsSubsystem::CancelAll()
{
    if (LiveActions.Num() == 0)
    {
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoElevenLabsSubsystem::CancelAll: cancelling %d live action(s)"),
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
        if (UInoElevenLabsTextToDialogueStream* Stream =
                Cast<UInoElevenLabsTextToDialogueStream>(Action))
        {
            Stream->CancelStream();
            continue;
        }

        UE_LOG(LogInoAgents, Warning,
               TEXT("UInoElevenLabsSubsystem::CancelAll: unknown live action type %s; "
                    "dropping reference without cancel"),
               *Action->GetClass()->GetName());
    }

    // Anything that didn't cleanly unregister itself above (shouldn't
    // happen in practice) is force-dropped now so GC can collect it.
    LiveActions.Reset();
}

void UInoElevenLabsSubsystem::RegisterLiveAction(UBlueprintAsyncActionBase* Action)
{
    if (Action == nullptr)
    {
        return;
    }
    LiveActions.Add(Action);
}

void UInoElevenLabsSubsystem::UnregisterLiveAction(UBlueprintAsyncActionBase* Action)
{
    if (Action == nullptr)
    {
        return;
    }
    LiveActions.Remove(Action);
}
