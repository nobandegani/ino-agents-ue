// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "InoDownloader.h"          // FInoDownloadProgress
#include "NeuTTS/InoNeuTTSTypes.h"  // FInoNeuTTSConfig

#include "InoNeuTTSLoadTest.generated.h"

class UInoNeuTTSSubsystem;

/**
 * Observer UObject for the `Ino.NeuTTS.LoadTest` smoke command.
 * NewObject + AddToRoot in the console command; the observer self-
 * unroots in HandleLoaded after logging the terminal result.
 *
 * Dynamic-delegate handlers MUST take FString by value (not
 * const-ref) — see the InoAgents CLAUDE.md observer-conventions
 * section for why.
 */
UCLASS()
class UInoNeuTTSLoadTestObserver : public UObject
{
    GENERATED_BODY()

public:
    /** Kick off LoadModelAsync against the given subsystem + config.
     *  Logs progress + terminal result via LogInoAgents. Removes self
     *  from root after OnLoaded fires (or its analog on synchronous
     *  early-out via the subsystem). */
    void Begin(UInoNeuTTSSubsystem* InSubsys, const FInoNeuTTSConfig& InConfig);

    UFUNCTION()
    void HandleDownloadProgress(const FInoDownloadProgress& Progress);

    UFUNCTION()
    void HandleLoaded(bool bSuccess, FString ErrorMessage);

private:
    UPROPERTY()
    TObjectPtr<UInoNeuTTSSubsystem> Subsys = nullptr;

    double TStartSeconds = 0.0;
    int32  ProgressLogStride = 0;  // throttle progress logs (every Nth tick)
};
