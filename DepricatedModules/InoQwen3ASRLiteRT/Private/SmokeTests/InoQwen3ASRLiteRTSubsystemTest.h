// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Qwen3ASR/InoQwen3ASRLiteRTTypes.h"
#include "InoQwen3ASRLiteRTSubsystemTest.generated.h"

/**
 * Observer UCLASS that owns the load + transcribe delegate handlers for
 * Ino.Qwen3ASRLiteRT.SubsystemTest. AddToRoot keeps it alive across the
 * async chain; UnRoot + ConditionalBeginDestroy clean up after the result
 * is logged.
 */
UCLASS()
class UInoQwen3ASRSubsystemTestObserver : public UObject
{
    GENERATED_BODY()

public:
    /** WAV path resolved by the console command before Activate(). */
    FString WavPath;

    /** Begin the load-then-transcribe chain. Holds a TWeakObjectPtr on the subsystem. */
    void Activate(class UInoQwen3ASRLiteRTSubsystem* Subsystem);

private:
    UFUNCTION()
    void HandleLoadComplete(bool bSuccess, FString ErrorMessage);

    UFUNCTION()
    void HandleTranscribeComplete(bool bSuccess, FInoQwen3ASRTranscribeResult Result, FString ErrorMessage);

    void Finish(const TCHAR* Phase, bool bOk, const FString& Message);

    TWeakObjectPtr<class UInoQwen3ASRLiteRTSubsystem> WeakSubsystem;
    double T0Activate = 0.0;
};
