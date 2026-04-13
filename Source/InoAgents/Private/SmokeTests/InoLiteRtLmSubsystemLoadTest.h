// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "LiteRtLm/InoLiteRtLmTypes.h"

#include "InoLiteRtLmSubsystemLoadTest.generated.h"

class UInoLiteRtLmSubsystem;


/**
 * One-shot observer for the Ino.LiteRtLm.SubsystemLoadTest console
 * command. Holds a UFUNCTION callback that BindDynamic can target, plus a
 * UPROPERTY-kept-alive reference to the subsystem and model config so they
 * are not garbage-collected mid-load.
 *
 * Added to the GC root when the test starts, removed from root inside
 * HandleLoaded so the observer itself (and its held references) become
 * eligible for collection once the load completes.
 */
UCLASS()
class UInoLiteRtLmSubsystemLoadTestObserver : public UObject
{
    GENERATED_BODY()

public:
    double StartTime = 0.0;

    
    TObjectPtr<UInoLiteRtLmSubsystem> Subsystem = nullptr;

    
    FInoLiteRtLmModelConfig Config;

    // NOTE: parameter types MUST exactly match the delegate's declaration
    // in InoLiteRtLmTypes.h (`bool, FString`). Dynamic delegate BindDynamic
    // does strict method-pointer matching — `const FString&` would fail to
    // compile even though it is the idiomatic way to pass read-only strings
    // elsewhere in C++. Use `FString` by value in every dynamic delegate
    // handler throughout the plugin.
    UFUNCTION()
    void HandleLoaded(bool bSuccess, FString ErrorMessage);
};
