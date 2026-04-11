// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "LiteRtLm/LiteRtLmTypes.h"

#include "InoAgentsLiteRtLmConversationSendTest.generated.h"

class ULiteRtLmSubsystem;
class ULiteRtLmModelConfig;
class ULiteRtLmConversation;

/**
 * One-shot observer for the InoAgents.LiteRtLm.ConversationSendTest console
 * command. Orchestrates:
 *   1. Load model via the subsystem (async).
 *   2. Create a conversation via the subsystem's factory.
 *   3. Bind OnComplete + OnError to the observer's UFUNCTION handlers.
 *   4. SendMessageAsync with the test prompt.
 *   5. On OnComplete / OnError, log result and release the observer
 *      (which in turn releases the conversation).
 *
 * Holds UPROPERTY references to the subsystem, config, and conversation so
 * none of them get garbage-collected mid-test. AddToRoot keeps the observer
 * itself alive. RemoveFromRoot in the final handler releases everything.
 *
 * Every handler takes FString by value (not const FString&) to match the
 * dynamic delegate binding convention documented in the D.1 observer.
 */
UCLASS()
class UInoAgentsLiteRtLmConversationSendTestObserver : public UObject
{
    GENERATED_BODY()

public:
    double StartTime = 0.0;
    FString Prompt;

    UPROPERTY()
    TObjectPtr<ULiteRtLmSubsystem> Subsystem = nullptr;

    UPROPERTY()
    TObjectPtr<ULiteRtLmModelConfig> Config = nullptr;

    UPROPERTY()
    TObjectPtr<ULiteRtLmConversation> Conversation = nullptr;

    // Fires after LoadModelAsync (or directly if the model is already
    // loaded — the test simulates this by calling HandleModelLoaded(true)
    // synchronously).
    UFUNCTION()
    void HandleModelLoaded(bool bSuccess, FString ErrorMessage);

    // Fires after the conversation's SendMessageAsync finishes successfully.
    UFUNCTION()
    void HandleConversationComplete(FString FullText);

    // Fires after the conversation's SendMessageAsync fails.
    UFUNCTION()
    void HandleConversationError(FString ErrorMessage);

private:
    /**
     * Release all references and detach from the GC root. Called from
     * HandleConversationComplete / HandleConversationError / failure paths
     * in HandleModelLoaded. Intentionally does NOT unload the model —
     * leaving the model loaded lets subsequent test runs reuse it, and
     * sidesteps the conversation/engine lifetime race that calling
     * UnloadModel while a conversation is still being GC'd would
     * otherwise produce.
     */
    void Finish();
};
