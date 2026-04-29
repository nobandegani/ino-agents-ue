// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"

class FJsonObject;
class UInoLiteRtLmSubsystem;
class UInoElevenLabsSubsystem;
class UInoNeuTtsNanoSubsystem;

/**
 * Helpers shared between smoke test console commands across the
 * InoAgents plugin and its sub-modules. Exposed under Public/ so
 * sub-modules (e.g. InoChatterboxOnnx) can reuse the generic
 * FindGameInstanceSubsystem<T>() template without depending on
 * InoAgents core's Private/.
 *
 * All functions log via LogInoAgents (see InoAgentsLog.h).
 */
namespace InoSmokeTest
{
    /**
     * Walk GEngine->GetWorldContexts() and return the first
     * UGameInstance's subsystem of type T (typically the PIE game
     * instance). Returns nullptr if GEngine is null, no world contexts
     * exist, or no context has the subsystem available yet.
     *
     * Defined inline because templates need definitions in the header
     * for callers across module boundaries.
     */
    template <typename TSubsystem>
    TSubsystem* FindGameInstanceSubsystem()
    {
        if (GEngine == nullptr)
        {
            return nullptr;
        }
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (UGameInstance* GI = Context.OwningGameInstance)
            {
                if (TSubsystem* Subsys = GI->GetSubsystem<TSubsystem>())
                {
                    return Subsys;
                }
            }
        }
        return nullptr;
    }

    /**
     * Typed wrappers for subsystems that live in InoAgents core. Sub-
     * modules whose subsystem lives elsewhere (e.g. Chatterbox via
     * InoChatterboxOnnx) should call FindGameInstanceSubsystem<T>()
     * directly to avoid pulling InoAgents core into a dep cycle.
     */
    INOAGENTS_API UInoLiteRtLmSubsystem*  FindLiteRtLmSubsystem();
    INOAGENTS_API UInoElevenLabsSubsystem* FindElevenLabsSubsystem();
    INOAGENTS_API UInoNeuTtsNanoSubsystem* FindNeuTtsNanoSubsystem();

    /**
     * Resolve the default phase-1 Gemma 4 E2B model path:
     *     Plugins/InoAgents/Models/gemma-4-E2B-it.litertlm
     *
     * On success, returns the absolute path. On failure (plugin not
     * found, file missing), logs a helpful error (with the HuggingFace
     * download URL for the missing-file case) and returns an empty
     * FString. Callers should treat an empty return as "bail out".
     */
    INOAGENTS_API FString ResolveDefaultModelPath();

    /**
     * Parse a JSON string into an FJsonObject. On failure, logs the
     * failure (using the caller-supplied Context as a prefix) and
     * returns null.
     */
    INOAGENTS_API TSharedPtr<FJsonObject> ParseJsonObjectOrLog(
        const FString& Json, const TCHAR* Context);

    /**
     * Extract the first tool call from an assistant response JSON
     * object. Returns true on success with the tool's name in
     * OutToolName and its arguments object in OutArgumentsObj.
     *
     * Format expected (matches LiteRT-LM's output):
     *   {
     *     "role": "assistant",
     *     "tool_calls": [
     *       { "type": "function",
     *         "function": { "name": "<name>", "arguments": { ... } } }
     *     ]
     *   }
     */
    INOAGENTS_API bool TryExtractFirstToolCall(
        const TSharedPtr<FJsonObject>& ResponseObj,
        FString& OutToolName,
        TSharedPtr<FJsonObject>& OutArgumentsObj);

    /**
     * Concatenate every `{"type":"text","text":"..."}` content part in
     * an assistant response into a single space-separated FString, for
     * logging. Diagnostic only — not a faithful rendering.
     */
    INOAGENTS_API FString ExtractAssistantText(const TSharedPtr<FJsonObject>& ResponseObj);
}
