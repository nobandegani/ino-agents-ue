// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "ElevenLabs/InoElevenLabsTypes.h"

#include "InoAgentsSettings.generated.h"

/**
 * Project-wide InoAgents core configuration (ElevenLabs cloud TTS).
 *
 * Visible under Edit -> Project Settings -> Plugins -> InoAgents.
 * Persisted to Config/DefaultGame.ini under
 * [/Script/InoAgents.InoAgentsSettings].
 *
 * Sub-module-specific settings live in their own UDeveloperSettings
 * pages owned by their module (UInoLiteRtLmSettings,
 * UInoChatterboxNativeSettings, UInoNeuTtsNativeSettings) — that way
 * deleting a sub-module's directory + its .uplugin entry also removes
 * its settings page automatically.
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
    // Accessors
    // =============================================================

    static const UInoAgentsSettings* Get() { return GetDefault<UInoAgentsSettings>(); }

    /** Effective ElevenLabs base URL (override or hardcoded default).
     *  Trailing slashes are stripped. */
    FString GetEffectiveElevenLabsBaseUrl() const;
};
