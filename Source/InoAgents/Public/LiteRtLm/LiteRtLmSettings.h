// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "LiteRtLm/LiteRtLmTypes.h"

#include "LiteRtLmSettings.generated.h"

/**
 * Project-wide LiteRT-LM configuration.
 *
 * Visible under Edit -> Project Settings -> Plugins -> InoAgents LiteRT-LM.
 * Persisted to Config/DefaultGame.ini under
 * [/Script/InoAgents.LiteRtLmSettings].
 *
 * The Models array maps model filenames to download URLs. The agent
 * component looks up the filename here when it needs to download a
 * model that isn't cached locally yet.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "InoAgents LiteRT-LM"))
class INOAGENTS_API ULiteRtLmSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    ULiteRtLmSettings();

    //~ UDeveloperSettings interface
    virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
    //~ End UDeveloperSettings interface

    /**
     * Available models and their download URLs. The agent component
     * matches its ModelConfig.ModelFileName against this array to find
     * the download URL when the model isn't on disk yet.
     *
     * Default entries point at the public Hugging Face repos for
     * Gemma 4 E2B and E4B. Add your own entries for custom or
     * fine-tuned models hosted on your own CDN.
     */
    UPROPERTY(EditAnywhere, Config, Category = "LiteRT-LM|Models")
    TArray<FLiteRtLmModelEntry> Models;

    /** Convenience shortcut for reading the CDO. */
    static const ULiteRtLmSettings* Get() { return GetDefault<ULiteRtLmSettings>(); }

    /** Look up a model entry by filename. Returns nullptr if not found. */
    const FLiteRtLmModelEntry* FindModelByFileName(const FString& FileName) const;
};
