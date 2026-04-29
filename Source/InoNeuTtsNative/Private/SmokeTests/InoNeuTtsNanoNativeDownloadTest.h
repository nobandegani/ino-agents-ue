// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "NeuTtsNanoNative/InoNeuTtsNanoNativeTypes.h"

#include "InoNeuTtsNanoNativeDownloadTest.generated.h"

class UInoNeuTtsNanoNativeSubsystem;

/**
 * One-shot observer for the Ino.NeuTtsNanoNative.DownloadTest console
 * command. Holds UFUNCTION callbacks that BindDynamic can target, plus
 * UPROPERTY-kept-alive references to the subsystem and config so they
 * aren't garbage-collected mid-download.
 *
 * Logs progress as it arrives (Percent + MB/total), success/failure
 * at terminal, file-stat verification after success.
 */
UCLASS()
class UInoNeuTtsNanoNativeDownloadTestObserver : public UObject
{
    GENERATED_BODY()

public:
    double StartTime = 0.0;
    double LastProgressLogTime = 0.0;

    UPROPERTY()
    TObjectPtr<UInoNeuTtsNanoNativeSubsystem> Subsystem = nullptr;

    UPROPERTY()
    FInoNeuTtsNanoNativeModelConfig Config;

    // Dynamic delegate handlers — FString by value per BindDynamic contract.
    UFUNCTION()
    void HandleLoaded(bool bSuccess, FString ErrorMessage);

    UFUNCTION()
    void HandleProgress(float Percent, int64 BytesReceived, int64 TotalBytes, bool bCompleted);
};
