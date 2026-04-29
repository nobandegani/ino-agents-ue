// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "ChatterboxTurboNative/InoChatterboxTurboNativeTypes.h"  // EInoChatterboxTurboNativeVariant + FInoChatterboxTurboNativeModelEntry

#include "InoChatterboxNativeSettings.generated.h"

/**
 * Project-wide settings for the Chatterbox Turbo TTS (ONNX) sub-module.
 *
 * Visible under Edit -> Project Settings -> Plugins -> InoChatterboxNative.
 * Persisted to Config/DefaultGame.ini under
 * [/Script/InoChatterboxNative.InoChatterboxNativeSettings].
 *
 * Lives in this module rather than UInoAgentsSettings so the entire
 * ONNX-flavored Chatterbox implementation is a single deletable unit:
 * deleting the InoChatterboxNative folder + dropping the .uplugin entry
 * also removes this Project Settings page automatically.
 */
UCLASS(Config = Game, DefaultConfig,
       meta = (DisplayName = "InoChatterboxNative"))
class INOCHATTERBOXNATIVE_API UInoChatterboxNativeSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UInoChatterboxNativeSettings();

    //~ UDeveloperSettings interface
    virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
    //~ End UDeveloperSettings interface

    /** Available Chatterbox Turbo variants and their download URLs.
     *  UInoChatterboxTurboNativeSubsystem::LoadModelsAsync matches its
     *  FInoChatterboxTurboNativeModelConfig::Variant against this array to find the
     *  HuggingFace repo URL + revision to pull missing files from. */
    UPROPERTY(EditAnywhere, Config, Category = "Chatterbox|Models")
    TArray<FInoChatterboxTurboNativeModelEntry> ChatterboxModels;

    // =============================================================
    // Accessors
    // =============================================================

    static const UInoChatterboxNativeSettings* Get() { return GetDefault<UInoChatterboxNativeSettings>(); }

    /** Look up a Chatterbox model entry by variant. Returns nullptr if
     *  no matching entry is configured — UInoChatterboxTurboNativeSubsystem
     *  treats that as "no download URL for this variant" and fails
     *  LoadModelsAsync with a clear error. */
    const FInoChatterboxTurboNativeModelEntry* FindChatterboxModel(EInoChatterboxTurboNativeVariant Variant) const;
};
