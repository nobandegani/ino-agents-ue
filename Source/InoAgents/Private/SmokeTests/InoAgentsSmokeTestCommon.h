// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * Helpers shared between the phase-1 smoke test console commands in
 * Private/SmokeTests/. None of this is part of the plugin's public API —
 * it exists purely to keep the smoke test .cpp files short and consistent.
 *
 * All functions log via LogInoAgents (see InoAgentsLog.h).
 */
namespace InoAgentsSmokeTest
{
    /**
     * Resolve the default phase-1 Gemma 4 E2B model path:
     *     Plugins/InoAgents/Models/gemma-4-E2B-it.litertlm
     *
     * On success, returns the absolute path as an FString. Also verifies the
     * file exists on disk.
     *
     * On failure (plugin not found, or file missing), logs a helpful error
     * (including the HuggingFace download URL for the missing-file case) and
     * returns an empty FString. Callers should treat an empty return as
     * "bail out of the test".
     *
     * This function exists because all five phase-1 smoke tests use the same
     * hardcoded model file, and the "find it, check it exists, error cleanly
     * if missing" dance was duplicated inline five times in the pre-refactor
     * InoAgents.cpp.
     */
    FString ResolveDefaultModelPath();

    /**
     * Parse a JSON string into an FJsonObject. On failure, logs the failure
     * (using the caller-supplied Context string as a prefix so the log tells
     * you which smoke test had the problem) and returns a null TSharedPtr.
     */
    TSharedPtr<FJsonObject> ParseJsonObjectOrLog(const FString& Json, const TCHAR* Context);

    /**
     * Extract the first tool call from an assistant response JSON object.
     * Returns true on success with the tool's name in OutToolName and its
     * arguments object in OutArgumentsObj.
     *
     * Returns false if the response has no `tool_calls` array, or if the
     * first entry is malformed. OutArgumentsObj is always assigned — on
     * success it holds the parsed arguments (possibly an empty object for
     * zero-arg tools), on failure it is an empty shared object.
     *
     * Format expected (matches LiteRT-LM's output per
     * runtime/conversation/model_data_processor/gemma4_data_processor_test.cc):
     *
     *     {
     *       "role": "assistant",
     *       "tool_calls": [
     *         { "type": "function",
     *           "function": { "name": "<name>", "arguments": { ... } } }
     *       ]
     *     }
     */
    bool TryExtractFirstToolCall(const TSharedPtr<FJsonObject>& ResponseObj,
                                 FString& OutToolName,
                                 TSharedPtr<FJsonObject>& OutArgumentsObj);

    /**
     * Concatenate every `{"type":"text","text":"..."}` content part in an
     * assistant response into a single space-separated FString, for logging.
     * Returns an empty string if the response has no text content or no
     * content array.
     *
     * This is for DIAGNOSTIC display only — it is not a faithful rendering of
     * the assistant message. The real UE API layer parses content more
     * carefully.
     */
    FString ExtractAssistantText(const TSharedPtr<FJsonObject>& ResponseObj);
}
