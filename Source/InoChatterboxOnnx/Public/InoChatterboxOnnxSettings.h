// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "Chatterbox/InoChatterboxTypes.h"  // EInoChatterboxVariant + FInoChatterboxModelEntry

#include "InoChatterboxOnnxSettings.generated.h"

/**
 * Project-wide settings for the Chatterbox Turbo TTS (ONNX) sub-module.
 *
 * Visible under Edit -> Project Settings -> Plugins -> InoChatterboxOnnx.
 * Persisted to Config/DefaultGame.ini under
 * [/Script/InoChatterboxOnnx.InoChatterboxOnnxSettings].
 *
 * Lives in this module rather than UInoAgentsSettings so the entire
 * ONNX-flavored Chatterbox implementation is a single deletable unit:
 * deleting the InoChatterboxOnnx folder + dropping the .uplugin entry
 * also removes this Project Settings page automatically.
 */
UCLASS(Config = Game, DefaultConfig,
       meta = (DisplayName = "InoChatterboxOnnx"))
class INOCHATTERBOXONNX_API UInoChatterboxOnnxSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UInoChatterboxOnnxSettings();

    //~ UDeveloperSettings interface
    virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
    //~ End UDeveloperSettings interface

    /** Available Chatterbox Turbo variants and their download URLs.
     *  UInoChatterboxTtsSubsystem::LoadModelsAsync matches its
     *  FInoChatterboxModelConfig::Variant against this array to find the
     *  HuggingFace repo URL + revision to pull missing files from. */
    UPROPERTY(EditAnywhere, Config, Category = "Chatterbox|Models")
    TArray<FInoChatterboxModelEntry> ChatterboxModels;

    // =============================================================
    // Accessors
    // =============================================================

    static const UInoChatterboxOnnxSettings* Get() { return GetDefault<UInoChatterboxOnnxSettings>(); }

    /** Look up a Chatterbox model entry by variant. Returns nullptr if
     *  no matching entry is configured — UInoChatterboxTtsSubsystem
     *  treats that as "no download URL for this variant" and fails
     *  LoadModelsAsync with a clear error. */
    const FInoChatterboxModelEntry* FindChatterboxModel(EInoChatterboxVariant Variant) const;
};
