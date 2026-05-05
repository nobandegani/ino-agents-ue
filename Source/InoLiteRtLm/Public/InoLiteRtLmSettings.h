// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "LiteRtLm/InoLiteRtLmTypes.h"  // FInoLiteRtLmModelEntry

#include "InoLiteRtLmSettings.generated.h"

/**
 * Project-wide settings for the LiteRT-LM Gemma 4 sub-module.
 *
 * Visible under Edit -> Project Settings -> Plugins -> InoLiteRtLm.
 * Persisted to Config/DefaultGame.ini under
 * [/Script/InoLiteRtLm.InoLiteRtLmSettings].
 *
 * Lives in this module rather than UInoAgentsSettings so the entire
 * LiteRT-LM implementation is a single deletable unit: deleting the
 * InoLiteRtLm folder + dropping the .uplugin entry also removes this
 * Project Settings page automatically.
 */
UCLASS(Config = Game, DefaultConfig,
       meta = (DisplayName = "InoLiteRtLm"))
class INOLITERTLM_API UInoLiteRtLmSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UInoLiteRtLmSettings();

    //~ UDeveloperSettings interface
    virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
    //~ End UDeveloperSettings interface

    /** Available models and their download URLs. UInoLiteRtLmSubsystem::
     *  LoadModelAsync matches its FInoLiteRtLmModelConfig::ModelFileName
     *  against this array (DisplayName first, LocalFileName as fallback)
     *  to find the download URL when the model isn't on disk yet. */
    UPROPERTY(EditAnywhere, Config, Category = "LiteRT-LM|Models")
    TArray<FInoLiteRtLmModelEntry> Models;

    // =============================================================
    // Accessors
    // =============================================================

    static const UInoLiteRtLmSettings* Get() { return GetDefault<UInoLiteRtLmSettings>(); }

    /** Look up a model entry by its on-disk filename
     *  (`Entry.LocalFileName`). Returns nullptr if not found. */
    const FInoLiteRtLmModelEntry* FindModelByFileName(const FString& FileName) const;

    /** Look up a model entry by either its DisplayName ("Gemma 4 E2B") or
     *  its LocalFileName ("gemma-4-E2B-it.litertlm"). Case-insensitive.
     *  Returns nullptr if no entry matches either field.
     *
     *  This is the forgiving lookup used by
     *  UInoLiteRtLmSubsystem::LoadModelAsync so Blueprint users don't
     *  have to memorize the exact on-disk filename — they can use the
     *  friendlier display name and the subsystem canonicalizes
     *  transparently. */
    const FInoLiteRtLmModelEntry* FindModel(const FString& NameOrFileName) const;

    // =============================================================
    // Static path helpers (mirror UInoNeuTtsNativeSettings' pattern)
    // =============================================================

    /**
     * Directory where downloaded model files are stored:
     *   <FPaths::ProjectPersistentDownloadDir()>/InoAgents/Models/
     *
     * Per-user, sandboxed on mobile, persists across project reinstalls.
     */
    static FString GetModelsDir();

    /**
     * Resolve a downloaded model's full local path:
     *   <GetModelsDir()> / <LocalFileName>
     *
     * Pure path construction — does NOT check that the file exists on
     * disk. Use `LiteRtLmResolveModelPath` (in InoLiteRtLmTypes.h) for
     * the existence-checking variant that also walks the legacy
     * Plugins/InoAgents/Models/ fallback.
     */
    static FString ResolveLocalPath(const FString& LocalFileName);
};
