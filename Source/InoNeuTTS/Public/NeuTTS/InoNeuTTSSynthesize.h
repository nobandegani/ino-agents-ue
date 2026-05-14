// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"

#include "NeuTTS/InoNeuTTSTypes.h"

#include "InoNeuTTSSynthesize.generated.h"

/**
 * Blueprint async-action wrapper for `UInoNeuTTSSubsystem::SynthesizeAsync`.
 *
 * Displays in the Blueprint context menu as **"NeuTTS Synthesize"**
 * with three exec pins: `OnComplete`, `OnError`. Voice comes from the
 * subsystem's currently-set active voice — caller must have already
 * called `SetActiveVoiceAsync` (or the node fires `OnError`).
 *
 * Lifetime: standard `UBlueprintAsyncActionBase` pattern. The factory
 * creates the action, Activate dispatches the synth, the dynamic
 * callback fires OnComplete or OnError, then `SetReadyToDestroy()`
 * lets the engine GC the action.
 */
UCLASS()
class INONEUTTS_API UInoNeuTTSSynthesize : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    /** Fires on the game thread when synthesis succeeds. `Result.AudioSamples`
     *  carries 24 kHz mono int16 PCM little-endian bytes. */
    UPROPERTY(BlueprintAssignable)
    FOnInoNeuTTSSynthesisComplete OnComplete;

    /** Fires on the game thread when synthesis fails. `Result.ErrorMessage`
     *  describes what went wrong. Mutually exclusive with OnComplete. */
    UPROPERTY(BlueprintAssignable)
    FOnInoNeuTTSSynthesisComplete OnError;

    /** Factory — Blueprint context menu entry "NeuTTS Synthesize". */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS",
              meta = (BlueprintInternalUseOnly = "true",
                      WorldContext = "WorldContextObject",
                      DisplayName  = "NeuTTS Synthesize"))
    static UInoNeuTTSSynthesize* SynthesizeAsync(
        UObject* WorldContextObject,
        const FString& Text,
        const FInoNeuTTSOptions& Options);

    //~ UBlueprintAsyncActionBase
    virtual void Activate() override;
    //~ End UBlueprintAsyncActionBase

private:
    UPROPERTY()
    TObjectPtr<UObject> WorldContextObject_ = nullptr;

    FString           Text_;
    FInoNeuTTSOptions Options_;

    /** Dynamic handler bound to UInoNeuTTSSubsystem::SynthesizeAsync's
     *  OnComplete delegate. Routes to OnComplete or OnError based on
     *  `Result.bSuccess`, then marks the action ready to destroy. */
    UFUNCTION()
    void HandleComplete(const FInoNeuTTSResult& Result);
};
