// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "NeuTtsNano/InoNeuTtsNanoTypes.h"

#include "InoNeuTtsNanoDownloadTest.generated.h"

class UInoNeuTtsNanoSubsystem;

/**
 * One-shot observer for the Ino.NeuTtsNano.DownloadTest console
 * command. Holds UFUNCTION callbacks that BindDynamic can target, plus
 * UPROPERTY-kept-alive references to the subsystem and config so they
 * aren't garbage-collected mid-download.
 *
 * Logs progress as it arrives (Percent + MB/total), success/failure
 * at terminal, file-stat verification after success.
 */
UCLASS()
class UInoNeuTtsNanoDownloadTestObserver : public UObject
{
    GENERATED_BODY()

public:
    double StartTime = 0.0;
    double LastProgressLogTime = 0.0;

    UPROPERTY()
    TObjectPtr<UInoNeuTtsNanoSubsystem> Subsystem = nullptr;

    UPROPERTY()
    FInoNeuTtsNanoModelConfig Config;

    // Dynamic delegate handlers — FString by value per BindDynamic contract.
    UFUNCTION()
    void HandleLoaded(bool bSuccess, FString ErrorMessage);

    UFUNCTION()
    void HandleProgress(float Percent, int64 BytesReceived, int64 TotalBytes, bool bCompleted);
};
