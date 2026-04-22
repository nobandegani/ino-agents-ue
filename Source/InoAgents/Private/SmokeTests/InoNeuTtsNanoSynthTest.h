// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "NeuTtsNano/InoNeuTtsNanoTypes.h"

#include "InoNeuTtsNanoSynthTest.generated.h"

class UInoNeuTtsNanoSubsystem;

/**
 * Observer for the Ino.NeuTtsNano.SynthTest console command.
 *
 * Orchestrates a full load + synth + WAV-save flow across two
 * asynchronous hops:
 *
 *   1. LoadModelAsync (if not already loaded) -> HandleLoaded
 *      -> kicks off SynthesizeAsync
 *   2. SynthesizeAsync -> HandleSynthComplete
 *      -> writes WAV, logs stats, RemoveFromRoot
 *
 * Both handlers marshal onto the game thread automatically (dynamic
 * delegates). UPROPERTY references + AddToRoot on construction keep
 * the observer + its captured state alive across the two hops.
 */
UCLASS()
class UInoNeuTtsNanoSynthTestObserver : public UObject
{
    GENERATED_BODY()

public:
    double StartTime = 0.0;
    double LoadStartTime = 0.0;
    double SynthStartTime = 0.0;

    UPROPERTY()
    TObjectPtr<UInoNeuTtsNanoSubsystem> Subsystem = nullptr;

    /** Phonemes supplied by the caller (or the baked-in default). */
    FString Phonemes;

    /** Output WAV target path; logged on success. */
    FString OutputPath;

    /** Synthesis options — sensible NeuTTS defaults; caller can't
     *  override from the console in v1. */
    FInoNeuTtsNanoSynthesisOptions Options;

    // FString by value throughout per BindDynamic contract.
    UFUNCTION()
    void HandleLoaded(bool bSuccess, FString ErrorMessage);

    UFUNCTION()
    void HandleSynthComplete(
        bool bSuccess,
        TArray<uint8> PcmInt16LE,
        int32 SampleRate,
        FString ErrorMessage);

    void KickOffSynthesis();
    void Finish();
};
