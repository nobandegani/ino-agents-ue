// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "NeuTTS/InoNeuTTSTypes.h"  // FInoNeuTTS*Entry structs

#include "InoNeuTTSSettings.generated.h"

/**
 * Project-wide settings for the InoNeuTTS module.
 *
 * Visible under Edit → Project Settings → Plugins → InoNeuTTS.
 * Persisted to Config/DefaultGame.ini under
 * [/Script/InoNeuTTS.InoNeuTTSSettings].
 *
 * Lives in this module rather than UInoAgentsSettings so the entire
 * NeuTTS implementation is a single deletable unit: deleting the
 * InoNeuTTS folder + dropping the .uplugin entry also removes this
 * Project Settings page automatically. Same pattern as InoLiteRtLm.
 */
UCLASS(Config = Game, DefaultConfig,
       meta = (DisplayName = "InoNeuTTS"))
class INONEUTTS_API UInoNeuTTSSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UInoNeuTTSSettings();

    //~ UDeveloperSettings interface
    virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
    //~ End UDeveloperSettings interface

    // =============================================================
    // Model registries
    // =============================================================

    /** Available NeuTTS Nano (.litertlm) backbones and their download
     *  URLs. UInoNeuTTSSubsystem::LoadModelAsync matches its
     *  FInoNeuTTSConfig::BackboneModelName against this array
     *  (DisplayName first, LocalFileName as fallback, case-insensitive). */
    UPROPERTY(EditAnywhere, Config, Category = "NeuTTS|Models")
    TArray<FInoNeuTTSBackboneEntry> BackboneModels;

    /** Available NeuCodec (.tflite) decoders and their download URLs.
     *  Decoder is language-agnostic — one decoder handles every
     *  backbone variant. */
    UPROPERTY(EditAnywhere, Config, Category = "NeuTTS|Models")
    TArray<FInoNeuTTSDecoderEntry> DecoderModels;

    // =============================================================
    // Accessors
    // =============================================================

    static const UInoNeuTTSSettings* Get() { return GetDefault<UInoNeuTTSSettings>(); }

    /** Look up a backbone entry by DisplayName OR LocalFileName,
     *  case-insensitive. Empty `NameOrFileName` returns the first
     *  entry. Returns nullptr if no entry matches. */
    const FInoNeuTTSBackboneEntry* FindBackbone(const FString& NameOrFileName) const;

    /** Same shape as FindBackbone, for the decoder array. */
    const FInoNeuTTSDecoderEntry* FindDecoder(const FString& NameOrFileName) const;

    // =============================================================
    // Static path helpers (mirror UInoLiteRtLmSettings' pattern)
    // =============================================================

    /**
     * Directory where downloaded model files are stored:
     *   <FPaths::ProjectPersistentDownloadDir()>/InoAgents/NeuTTS/
     *
     * Per-user, sandboxed on mobile, persists across project reinstalls.
     * Backbones and decoders share this directory; their LocalFileName
     * fields disambiguate.
     */
    static FString GetModelsDir();

    /**
     * Resolve a downloaded model's full local path:
     *   <GetModelsDir()> / <LocalFileName>
     *
     * Pure path construction — does NOT check that the file exists on
     * disk.
     */
    static FString ResolveLocalPath(const FString& LocalFileName);
};
