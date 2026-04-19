// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "ElevenLabs/InoElevenLabsTypes.h"
#include "LiteRtLm/InoLiteRtLmTypes.h"

#include "InoAgentsSettings.generated.h"

/**
 * Project-wide InoAgents configuration.
 *
 * Visible under Edit -> Project Settings -> Plugins -> InoAgents.
 * One page with two sections: ElevenLabs and LiteRT-LM.
 * Persisted to Config/DefaultGame.ini under
 * [/Script/InoAgents.InoAgentsSettings].
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "InoAgents"))
class INOAGENTS_API UInoAgentsSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UInoAgentsSettings();

    //~ UDeveloperSettings interface
    virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
    //~ End UDeveloperSettings interface

    // =============================================================
    // ElevenLabs
    // =============================================================

    /** xi-api-key for ElevenLabs. Get yours at
     *  https://elevenlabs.io/app/settings/api-keys.
     *  Stored plaintext in DefaultGame.ini — do not commit a real key. */
    UPROPERTY(EditAnywhere, Config, Category = "ElevenLabs",
              meta = (DisplayName = "API Key", PasswordField = "true"))
    FString ElevenLabsApiKey;

    /** Optional base URL override for regional routing. Empty =
     *  https://api.elevenlabs.io. */
    UPROPERTY(EditAnywhere, Config, Category = "ElevenLabs",
              meta = (DisplayName = "Base URL"))
    FString ElevenLabsBaseUrl;

    /** Default model id when FInoElevenLabsDialogueRequest::ModelId is empty. */
    UPROPERTY(EditAnywhere, Config, Category = "ElevenLabs|Defaults")
    FString ElevenLabsDefaultModelId = TEXT("eleven_v3");

    /** Default audio format when no explicit format is passed. */
    UPROPERTY(EditAnywhere, Config, Category = "ElevenLabs|Defaults")
    EInoElevenLabsOutputFormat ElevenLabsDefaultOutputFormat = EInoElevenLabsOutputFormat::Mp3_44100_128;

    // =============================================================
    // LiteRT-LM
    // =============================================================

    /** Available models and their download URLs. The agent component
     *  matches its ModelConfig.ModelFileName against this array to find
     *  the download URL when the model isn't on disk yet. */
    UPROPERTY(EditAnywhere, Config, Category = "LiteRT-LM|Models")
    TArray<FInoLiteRtLmModelEntry> Models;

    // =============================================================
    // Accessors
    // =============================================================

    static const UInoAgentsSettings* Get() { return GetDefault<UInoAgentsSettings>(); }

    /** Effective ElevenLabs base URL (override or hardcoded default).
     *  Trailing slashes are stripped. */
    FString GetEffectiveElevenLabsBaseUrl() const;

    /** Look up a model entry by filename. Returns nullptr if not found. */
    const FInoLiteRtLmModelEntry* FindModelByFileName(const FString& FileName) const;

    /** Look up a model entry by either its DisplayName ("Gemma 4 E2B") or its
     *  ModelFileName ("gemma-4-E2B-it.litertlm"). Case-insensitive. Returns
     *  nullptr if no entry matches either field. This is the forgiving
     *  lookup used by UInoLiteRtLmSubsystem::LoadModelAsync so Blueprint
     *  users don't have to memorize the exact on-disk filename — they can
     *  use the friendlier display name and the subsystem canonicalizes
     *  transparently. */
    const FInoLiteRtLmModelEntry* FindModel(const FString& NameOrFileName) const;
};
