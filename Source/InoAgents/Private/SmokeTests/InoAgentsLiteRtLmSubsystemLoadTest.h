// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "LiteRtLm/LiteRtLmTypes.h"

#include "InoAgentsLiteRtLmSubsystemLoadTest.generated.h"

class ULiteRtLmSubsystem;
class ULiteRtLmModelConfig;

/**
 * One-shot observer for the InoAgents.LiteRtLm.SubsystemLoadTest console
 * command. Holds a UFUNCTION callback that BindDynamic can target, plus a
 * UPROPERTY-kept-alive reference to the subsystem and model config so they
 * are not garbage-collected mid-load.
 *
 * Added to the GC root when the test starts, removed from root inside
 * HandleLoaded so the observer itself (and its held references) become
 * eligible for collection once the load completes.
 */
UCLASS()
class UInoAgentsLiteRtLmSubsystemLoadTestObserver : public UObject
{
    GENERATED_BODY()

public:
    double StartTime = 0.0;

    UPROPERTY()
    TObjectPtr<ULiteRtLmSubsystem> Subsystem = nullptr;

    UPROPERTY()
    TObjectPtr<ULiteRtLmModelConfig> Config = nullptr;

    UFUNCTION()
    void HandleLoaded(bool bSuccess, const FString& ErrorMessage);
};
