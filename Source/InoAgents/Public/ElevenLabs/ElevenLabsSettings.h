// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "ElevenLabs/ElevenLabsTypes.h"

#include "ElevenLabsSettings.generated.h"

/**
 * Project-wide ElevenLabs configuration.
 *
 * Visible under Edit -> Project Settings -> Plugins -> InoAgents ElevenLabs.
 * Persisted to Config/DefaultGame.ini under
 * [/Script/InoAgents.ElevenLabsSettings].
 *
 * SECURITY NOTE: the API key is stored as plaintext in the ini file. Do
 * NOT commit a real key to a public repo. Treat it like any other dev
 * secret - add DefaultGame.ini overrides to your local .gitignore, or use
 * the per-call override parameter on the async action to pass a key
 * fetched from your own secret store at runtime.
 *
 * All fields are cached once per PIE session by UElevenLabsSubsystem at
 * its Initialize() call; to pick up a changed key without restarting PIE,
 * run the InoAgents.ElevenLabs.ReloadSettings console command.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "InoAgents ElevenLabs"))
class INOAGENTS_API UElevenLabsSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    //~ UDeveloperSettings interface
    virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
    //~ End UDeveloperSettings interface

    /**
     * xi-api-key for ElevenLabs. Get yours at
     * https://elevenlabs.io/app/settings/api-keys.
     *
     * Stored plaintext in DefaultGame.ini - do not commit a real key.
     */
    UPROPERTY(EditAnywhere, Config, Category = "ElevenLabs",
              meta = (DisplayName = "API Key", PasswordField = "true"))
    FString ApiKey;

    /**
     * Optional base URL override for regional routing. Empty resolves to
     * https://api.elevenlabs.io. Valid alternates per ElevenLabs docs:
     *   https://api.us.elevenlabs.io
     *   https://api.eu.residency.elevenlabs.io
     *   https://api.in.residency.elevenlabs.io
     */
    UPROPERTY(EditAnywhere, Config, Category = "ElevenLabs",
              meta = (DisplayName = "Base URL"))
    FString BaseUrl;

    /** Default model id when FElevenLabsDialogueRequest::ModelId is empty. */
    UPROPERTY(EditAnywhere, Config, Category = "ElevenLabs|Defaults")
    FString DefaultModelId = TEXT("eleven_v3");

    /** Default audio format when no explicit format is passed. */
    UPROPERTY(EditAnywhere, Config, Category = "ElevenLabs|Defaults")
    EElevenLabsOutputFormat DefaultOutputFormat = EElevenLabsOutputFormat::Mp3_44100_128;

    /** Convenience shortcut for reading the CDO directly (settings live on the CDO). */
    static const UElevenLabsSettings* Get() { return GetDefault<UElevenLabsSettings>(); }

    /** Returns the effective base URL: the override if set, otherwise the hardcoded default. */
    FString GetEffectiveBaseUrl() const;
};
