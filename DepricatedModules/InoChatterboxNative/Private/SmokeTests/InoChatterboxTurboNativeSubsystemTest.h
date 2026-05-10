// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "ChatterboxTurboNative/InoChatterboxTurboNativeTypes.h"

#include "InoChatterboxTurboNativeSubsystemTest.generated.h"

class UInoChatterboxTurboNativeSubsystem;

/**
 * One-shot observer UCLASS for the Ino.Chatterbox.SubsystemSynthTest
 * console command.
 *
 * Holds UFUNCTION handlers for the three delegates we bind during the
 * test (OnLoaded, OnComplete, OnDownloadProgress) plus UPROPERTY-kept
 * references to the subsystem and the in-flight config so nothing gets
 * GC'd mid-load.
 *
 * Added to the GC root when the test starts; removed from root inside
 * HandleComplete (or HandleLoaded on failure) so the observer itself
 * and its held references become eligible for collection once the
 * workflow finishes.
 *
 * Dynamic-delegate handler quirk: parameter types MUST match the
 * delegate declaration in InoChatterboxTurboNativeTypes.h exactly. FString is by
 * value everywhere throughout the plugin — const FString& fails to
 * compile at BindDynamic with a cryptic "cannot convert argument"
 * error.
 */
UCLASS()
class UInoChatterboxTurboNativeSubsystemTestObserver : public UObject
{
    GENERATED_BODY()

public:
    /** Start timestamp, used to compute wall-clock duration. */
    double StartTime = 0.0;

    /** The subsystem we're testing. Kept alive via UPROPERTY. */
    UPROPERTY()
    TObjectPtr<UInoChatterboxTurboNativeSubsystem> Subsystem = nullptr;

    /** Reference-voice WAV path on disk, resolved before load. Feeds
     *  SynthesizeAsync once the load succeeds. */
    FString VoiceWavPath;

    /** Prompt text — captured from the command arg list, or the built-in
     *  default if the user didn't pass one. */
    FString PromptText;

    /** Where to save the generated WAV for listening-test inspection. */
    FString OutputWavPath;

    /** Max new tokens, forwarded to FInoChatterboxTurboNativeSynthesisOptions. */
    int32 MaxNewTokens = 512;

    /** Called when LoadModelsAsync completes. Kicks off SynthesizeAsync
     *  on success; logs + unroot on failure. */
    UFUNCTION()
    void HandleLoaded(bool bSuccess, FString ErrorMessage);

    /** Called when SynthesizeAsync completes. Writes the WAV, logs the
     *  result, unloads the model, unroots. */
    UFUNCTION()
    void HandleSynthComplete(
        bool bSuccess,
        FInoChatterboxTurboNativeSynthesisResult Result,
        FString ErrorMessage);
};
