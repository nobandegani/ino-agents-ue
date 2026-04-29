// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "NeuTtsNano/InoNeuTtsNanoTypes.h"  // EInoNeuTtsNanoBackboneVariant + FInoNeuTtsNanoModelEntry

#include "InoNeuTtsNativeSettings.generated.h"

/**
 * Project-wide settings for the NeuTTS Nano TTS sub-module.
 *
 * Visible under Edit -> Project Settings -> Plugins -> InoNeuTtsNative.
 * Persisted to Config/DefaultGame.ini under
 * [/Script/InoNeuTtsNative.InoNeuTtsNativeSettings].
 *
 * Lives in this module rather than UInoAgentsSettings so the entire
 * NeuTTS Nano implementation is a single deletable unit: deleting the
 * InoNeuTtsNative folder + dropping the .uplugin entry also removes
 * this Project Settings page automatically.
 */
UCLASS(Config = Game, DefaultConfig,
       meta = (DisplayName = "InoNeuTtsNative"))
class INONEUTTSNATIVE_API UInoNeuTtsNativeSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UInoNeuTtsNativeSettings();

    //~ UDeveloperSettings interface
    virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
    //~ End UDeveloperSettings interface

    /** Available NeuTTS Nano variants and their download URLs.
     *  UInoNeuTtsNanoSubsystem::LoadModelAsync matches its
     *  FInoNeuTtsNanoModelConfig::Variant against this array to find
     *  the backbone GGUF + codec ONNX URLs to pull missing files from.
     *  Each entry carries TWO HuggingFace repo URLs — one for the
     *  Qwen2-derived backbone and one for the NeuCodec ONNX decoder. */
    UPROPERTY(EditAnywhere, Config, Category = "NeuTTS Nano|Models")
    TArray<FInoNeuTtsNanoModelEntry> NeuTtsNanoModels;

    // =============================================================
    // Accessors
    // =============================================================

    static const UInoNeuTtsNativeSettings* Get() { return GetDefault<UInoNeuTtsNativeSettings>(); }

    /** Look up a NeuTTS Nano model entry by backbone variant. Returns
     *  nullptr if no matching entry is configured — UInoNeuTtsNanoSubsystem
     *  treats that as "no download URL for this variant" and fails
     *  LoadModelAsync with a clear error. */
    const FInoNeuTtsNanoModelEntry* FindNeuTtsNanoModel(EInoNeuTtsNanoBackboneVariant Variant) const;
};
