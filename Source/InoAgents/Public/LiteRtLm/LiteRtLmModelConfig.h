// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "LiteRtLm/LiteRtLmTypes.h"

#include "LiteRtLmModelConfig.generated.h"

/**
 * Designer-editable configuration for a LiteRT-LM model.
 *
 * Create one of these as a Content Browser asset (right-click → Miscellaneous
 * → Data Asset → ULiteRtLmModelConfig) per model you want to ship. Point
 * LoadModelAsync at it to load the model.
 *
 * IMPORTANT: tool calling via constrained decoding is currently Gemma-family-
 * only because libGemmaModelConstraintProvider.dll is shipped only for Gemma
 * models. Pointing this config at a non-Gemma model (Qwen, generic, etc.)
 * will work for chat but may produce unreliable tool calling.
 */
UCLASS(BlueprintType)
class INOAGENTS_API ULiteRtLmModelConfig : public UDataAsset
{
    GENERATED_BODY()

public:
    /**
     * Filename of the .litertlm model file, resolved relative to the plugin's
     * Models/ directory. Example: "gemma-4-E2B-it.litertlm".
     *
     * Not a full path — the subsystem prepends
     *   IPluginManager::FindPlugin("InoAgents")->GetBaseDir() + "/Models/".
     * This keeps configs portable across developer machines.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="InoAgents|LiteRT-LM")
    FString ModelFileName = TEXT("gemma-4-E2B-it.litertlm");

    /**
     * Which backend the engine should use.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="InoAgents|LiteRT-LM")
    ELiteRtLmBackend Backend = ELiteRtLmBackend::Cpu;

    /**
     * Upper bound on tokens per decode step. Zero means "use engine default".
     * Only meaningful for some backends.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="InoAgents|LiteRT-LM",
              meta=(ClampMin="0"))
    int32 MaxNumTokens = 0;

    /**
     * Optional system message applied to conversations created from this
     * config. Plain text. The subsystem wraps it in the expected
     * {"type":"text","text":"..."} JSON shape before handing it to
     * LiteRT-LM — do not include JSON braces here.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="InoAgents|LiteRT-LM",
              meta=(MultiLine=true))
    FString SystemMessage;
};
