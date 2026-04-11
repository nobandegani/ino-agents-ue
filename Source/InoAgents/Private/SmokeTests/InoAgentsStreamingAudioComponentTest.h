// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "Audio/InoAgentsAudioTypes.h"

#include "InoAgentsStreamingAudioComponentTest.generated.h"

class UInoAgentsStreamingAudioComponent;
class AActor;

/**
 * One-shot observer for the InoAgents.Audio.* console commands.
 *
 * Owns the spawned host actor + the streaming audio component,
 * binds the three delegates, and tears itself down in Finish().
 * Matches the observer-UCLASS pattern used throughout the plugin's
 * other smoke tests.
 *
 * Delegate handlers: OnReadyToPlay and OnFinished are no-arg; OnError
 * takes FString BY VALUE (matching the dynamic multicast convention).
 */
UCLASS()
class UInoAgentsStreamingAudioComponentTestObserver : public UObject
{
    GENERATED_BODY()

public:
    FString  TestName;
    double   StartTime = 0.0;
    double   ReadyTime = 0.0;
    bool     bReadyFired    = false;
    bool     bFinishedFired = false;
    bool     bErrored       = false;

    UPROPERTY()
    TObjectPtr<AActor> HostActor = nullptr;

    UPROPERTY()
    TObjectPtr<UInoAgentsStreamingAudioComponent> AudioComp = nullptr;

    UFUNCTION() void HandleReadyToPlay();
    UFUNCTION() void HandleFinished();
    UFUNCTION() void HandleError(FString ErrorMessage);

    void Finish();
};
