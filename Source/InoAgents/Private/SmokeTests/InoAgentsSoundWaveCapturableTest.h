// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "InoAgentsSoundWaveCapturableTest.generated.h"

class UInoAgentsCapturableSoundWave;

/**
 * Observer for InoAgents.SoundWave.Capture. Holds the wave under
 * test, tallies populate / started / stopped fires, and tears itself
 * down when the capture stream closes (or on error).
 */
UCLASS()
class UInoAgentsSoundWaveCaptureTestObserver : public UObject
{
    GENERATED_BODY()

public:
    double StartTime        = 0.0;
    int32  PopulateFires    = 0;
    int64  TotalSamplesSeen = 0;
    bool   bCaptureStarted  = false;

    UPROPERTY()
    TObjectPtr<UInoAgentsCapturableSoundWave> Wave = nullptr;

    UFUNCTION() void HandleStarted();
    UFUNCTION() void HandleStopped();
    UFUNCTION() void HandlePopulate(const TArray<float>& Data);
    UFUNCTION() void HandleError(FString ErrorMessage);
};
