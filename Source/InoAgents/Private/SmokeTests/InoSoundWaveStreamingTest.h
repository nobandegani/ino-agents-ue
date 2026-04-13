// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "InoSoundWaveStreamingTest.generated.h"

class UAudioComponent;
class UInoStreamingSoundWave;
class AActor;

/**
 * Observer for the InoAgents.SoundWave.Streaming* console commands.
 *
 * Owns the spawned host actor, the UAudioComponent, and the
 * UInoStreamingSoundWave, and binds the delegates under test.
 * Tears itself down in Finish() when the wave's
 * OnAudioPlaybackFinished fires (or OnAudioError aborts early).
 *
 * Dynamic delegate handlers take FString BY VALUE per the plugin's
 * BindDynamic convention (see Plugins/InoAgents/CLAUDE.md →
 * "UE API observer UCLASS pattern").
 */
UCLASS()
class UInoSoundWaveStreamingTestObserver : public UObject
{
    GENERATED_BODY()

public:
    FString  TestName;
    double   StartTime = 0.0;
    int32    PopulateFires      = 0;
    int32    GeneratePcmFires   = 0;
    int32    GeneratePcmSamples = 0;
    bool     bFinishedFired     = false;
    bool     bErrored           = false;

    UPROPERTY()
    TObjectPtr<AActor> HostActor = nullptr;

    UPROPERTY()
    TObjectPtr<UAudioComponent> AudioComp = nullptr;

    UPROPERTY()
    TObjectPtr<UInoStreamingSoundWave> Wave = nullptr;

    UFUNCTION() void HandlePopulate(const TArray<float>& Data);
    UFUNCTION() void HandleGeneratePcm(const TArray<float>& Data);
    UFUNCTION() void HandlePlaybackFinished();
    UFUNCTION() void HandleError(FString ErrorMessage);

    void Finish();
};
