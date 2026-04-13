// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "JsonObjectWrapper.h"

#include "InoLiteRtLmToolBase.generated.h"

/**
 * One parameter in a tool's schema. Rendered into the OpenAI-style
 * JSON schema by UInoLiteRtLmToolBase::BuildSchemaJson().
 */
USTRUCT(BlueprintType)
struct FInoLiteRtLmToolParameter
{
    GENERATED_BODY()

    /** Parameter name (snake_case). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Tool")
    FName Name;

    /** JSON Schema type: "string", "integer", "number", "boolean". */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Tool")
    FString Type = TEXT("string");

    /** Human-readable description for the model. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Tool",
              meta = (MultiLine = true))
    FString Description;

    /** If true, this parameter is listed in the schema's "required" array. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Tool")
    bool bRequired = true;
};

/**
 * Base class for LiteRT-LM tools. Subclass this in C++ or Blueprint,
 * set the properties (ToolName, Description, Parameters), and override
 * Execute to implement the tool's logic.
 *
 * The subsystem builds the OpenAI-style function-call JSON schema
 * automatically from the properties via BuildSchemaJson() — no
 * hand-written JSON needed.
 *
 * Blueprint workflow:
 *   1. Create a Blueprint subclass of this class.
 *   2. In Class Defaults, set ToolName, Description, and Parameters.
 *   3. Override the Execute function — use GetField nodes on the
 *      Arguments parameter to read the model's input.
 *   4. In BeginPlay, call RegisterTool on the LiteRtLm Subsystem,
 *      passing an instance of your tool.
 *
 * C++ workflow:
 *   1. Subclass UInoLiteRtLmToolBase.
 *   2. Set ToolName, Description, Parameters in the constructor.
 *   3. Override Execute_Implementation.
 *
 * Threading: Execute runs on the game thread. Implementations can
 * freely touch UObjects, actors, components, and world state.
 */
UCLASS(Blueprintable, BlueprintType, Abstract)
class INOAGENTS_API UInoLiteRtLmToolBase : public UObject
{
    GENERATED_BODY()

public:
    /** Unique identifier for this tool. The model sees this name
     *  verbatim when emitting a call. Use snake_case. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "InoAgents|Tool")
    FName ToolName;

    /** Human-readable description that tells the model when and how
     *  to use this tool. This is the single most important field for
     *  getting the model to actually call the tool. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "InoAgents|Tool",
              meta = (MultiLine = true))
    FString Description;

    /** Parameter definitions. Each entry becomes a property in the
     *  JSON schema's "parameters.properties" object. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "InoAgents|Tool")
    TArray<FInoLiteRtLmToolParameter> Parameters;

    /**
     * Execute the tool. Arguments is a parsed JSON object containing
     * the parameters the model supplied. Use GetField nodes in
     * Blueprint (from JsonBlueprintUtilities) to read values.
     *
     * Return values:
     *   - Plain text strings are auto-wrapped in JSON quotes by the
     *     conversation worker if needed.
     *   - Bare JSON numbers (e.g. "42") are passed through as-is.
     *   - JSON objects/arrays are passed through as-is.
     *
     * Override this in your subclass (C++ or Blueprint).
     */
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "InoAgents|Tool")
    FString Execute(const FJsonObjectWrapper& Arguments);
    virtual FString Execute_Implementation(const FJsonObjectWrapper& Arguments);

    /**
     * Internal: called by the conversation worker with the raw JSON
     * string from the model. Parses it into FJsonObjectWrapper and
     * calls Execute. Subclasses should NOT override this — override
     * Execute instead.
     */
    FString ExecuteFromString(const FString& ArgumentsJson);

    /**
     * Build the OpenAI-style function-call JSON schema from ToolName,
     * Description, and Parameters. Called by the subsystem during
     * RegisterTool and BuildToolsJsonForConversation.
     *
     * Override this only if you need a schema shape that the property-
     * driven builder can't express (e.g. nested objects, enums).
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Tool")
    virtual FString BuildSchemaJson() const;
};
