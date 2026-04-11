// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"

#include "LiteRtLmTool.generated.h"

/**
 * UInterface boilerplate. Never instantiate this — it exists only so
 * that UHT can generate reflection data for ILiteRtLmTool. Blueprint
 * and C++ classes implement ILiteRtLmTool, not ULiteRtLmTool.
 */
UINTERFACE(MinimalAPI, BlueprintType, meta=(CannotImplementInterfaceInBlueprint=false))
class ULiteRtLmTool : public UInterface
{
    GENERATED_BODY()
};

/**
 * Contract for a callable tool that a ULiteRtLmConversation can invoke
 * on behalf of the model during an agent loop.
 *
 * Tools are registered globally on the ULiteRtLmSubsystem via
 * RegisterTool. When a conversation is created, the subsystem serialises
 * every registered tool's schema into the `tools_json` parameter passed
 * to litert_lm_conversation_config_create. At runtime, when the model
 * emits a tool call during a streaming send, the conversation's worker
 * parses the call, looks up the tool by name in the registry, and calls
 * Execute on the game thread with the model-supplied arguments JSON.
 * The returned result JSON is then fed back into the conversation as a
 * {"role":"tool"} message so the model can use the result in its final
 * answer.
 *
 * Implementors provide three things:
 *   1. GetToolName()       — the unique identifier the model will use
 *                             when emitting a call.
 *   2. GetToolSchemaJson() — an OpenAI-style function-call schema
 *                             describing the tool's parameters.
 *   3. Execute(Args)       — the synchronous implementation that runs
 *                             on the game thread whenever the model
 *                             calls the tool.
 *
 * All three methods can be implemented in either C++ (via override of
 * the _Implementation variant) or Blueprint (via BlueprintNativeEvent).
 * The default C++ bodies in LiteRtLmTool.cpp are empty — every
 * implementor MUST override all three.
 *
 * Threading: Execute runs on the game thread. Tool implementations can
 * freely touch UE actors, components, world state, etc. without any
 * locking. The game thread is NOT blocked while the tool runs because
 * the conversation's SendMessageAsync is already async — the worker
 * thread queues a game-thread task to invoke Execute and blocks on an
 * FEvent until the task finishes, so the caller's tick never stalls
 * on model inference.
 *
 * Tools should be cheap. Tools that need to do their own async work
 * are not supported in Milestone D — the SubmitDeferredToolResult API
 * on the conversation is stubbed for a future iteration.
 */
class INOAGENTS_API ILiteRtLmTool
{
    GENERATED_BODY()

public:
    /**
     * Unique identifier for this tool. Must match the "name" field
     * inside GetToolSchemaJson()'s function object, or the subsystem
     * will refuse to register the tool.
     *
     * Use a short, snake_case identifier — the model sees it verbatim
     * when emitting a call.
     */
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="InoAgents|LiteRT-LM|Tool")
    FName GetToolName() const;
    virtual FName GetToolName_Implementation() const { return NAME_None; }

    /**
     * OpenAI-style function-call schema as a JSON STRING. The string
     * must parse to an object of the form:
     *
     *     {
     *       "type": "function",
     *       "function": {
     *         "name": "<matches GetToolName()>",
     *         "description": "<human-readable>",
     *         "parameters": {
     *           "type": "object",
     *           "properties": { ... },
     *           "required": [ ... ]
     *         }
     *       }
     *     }
     *
     * The subsystem's BuildToolsJsonForConversation() concatenates
     * every registered tool's schema into a JSON array and feeds
     * it to litert_lm_conversation_config_create. Schemas that do
     * not parse are logged and dropped.
     */
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="InoAgents|LiteRT-LM|Tool")
    FString GetToolSchemaJson() const;
    virtual FString GetToolSchemaJson_Implementation() const { return FString(); }

    /**
     * Execute the tool. ArgumentsJson is a JSON object (as a string)
     * matching the properties defined in the schema. Returns a JSON
     * value (number, string, object, array) as a string — the return
     * value is embedded verbatim in the tool_response.value field of
     * the follow-up message sent back to the model.
     *
     * Runs on the game thread. Implementors can freely touch UObjects,
     * actors, components, and world state without locking.
     *
     * If the tool encounters an error, it should return a JSON string
     * of the form `"ERROR: <message>"` (quoted JSON string literal).
     * The conversation forwards the error to the model verbatim; the
     * model is responsible for recovering, usually by explaining the
     * failure to the user.
     *
     * Implementors SHOULD NOT throw exceptions. If one escapes, the
     * conversation catches it and forwards `"ERROR: uncaught exception"`
     * to the model.
     */
    UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="InoAgents|LiteRT-LM|Tool")
    FString Execute(const FString& ArgumentsJson);
    virtual FString Execute_Implementation(const FString& ArgumentsJson) { return FString(); }
};
