// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "LiteRtLm/LiteRtLmTypes.h"

#include "InoAgentsLiteRtLmConversationToolTest.generated.h"

class ULiteRtLmSubsystem;

class ULiteRtLmConversation;
class ULiteRtLmAddNumbersTool;

/**
 * One-shot observer for the InoAgents.LiteRtLm.ConversationToolTest
 * console command (Milestone D.4b validation).
 *
 * Orchestrates the full agent loop:
 *   1. Load the model via the subsystem if not already loaded.
 *   2. Register a ULiteRtLmAddNumbersTool with the subsystem (at
 *      the top level of the test, BEFORE CreateConversation — tools
 *      registered after conversation creation do not apply).
 *   3. Create a conversation. The conversation's Initialize pulls
 *      tools_json from the subsystem and enables constrained
 *      decoding.
 *   4. Bind OnToken + OnToolCalled + OnComplete + OnError.
 *   5. SendMessageAsync with "What is 27 plus 15?".
 *   6. Expect: the model emits a tool_call for add_numbers, the
 *      worker executes the tool on the game thread, OnToolCalled
 *      fires once with tool name "add_numbers" and arguments
 *      {"a":27,"b":15} and result "42", then streaming continues
 *      into the final text response, OnComplete fires with a
 *      string containing "42" or "forty-two".
 *
 * Holds UPROPERTY refs to the subsystem, config, conversation, and
 * tool so nothing is garbage-collected mid-test. AddToRoot keeps
 * the observer alive. In Finish: Unregister the tool, Shutdown the
 * conversation, RemoveFromRoot.
 *
 * All delegate handlers take FString (and FName) by value to match
 * the dynamic delegate binding convention used throughout the
 * plugin (see the D.1 observer comment for the full rationale).
 */
UCLASS()
class UInoAgentsLiteRtLmConversationToolTestObserver : public UObject
{
    GENERATED_BODY()

public:
    double StartTime = 0.0;
    double FirstTokenTime = 0.0;
    int32  NumTokens = 0;
    int32  NumToolCalls = 0;

    FString Prompt;
    FString AccumulatedText;

    /** Set to true when HandleToolCalled has been called at least once
     *  with tool name "add_numbers" and result "42". Checked in
     *  HandleConversationComplete to flag the test PASS/FAIL. */
    bool bSawExpectedAddNumbersCall = false;

    
    TObjectPtr<ULiteRtLmSubsystem> Subsystem = nullptr;

    
    FLiteRtLmModelConfig Config;

    
    TObjectPtr<ULiteRtLmConversation> Conversation = nullptr;

    
    TObjectPtr<ULiteRtLmAddNumbersTool> Tool = nullptr;

    UFUNCTION()
    void HandleModelLoaded(bool bSuccess, FString ErrorMessage);

    UFUNCTION()
    void HandleToken(FString RawText, FString CleanText);

    UFUNCTION()
    void HandleToolCalled(FName ToolName, FString ArgumentsJson, FString ResultJson);

    UFUNCTION()
    void HandleConversationComplete(FString FullText);

    UFUNCTION()
    void HandleConversationError(FString ErrorMessage);

private:
    void Finish();
};
