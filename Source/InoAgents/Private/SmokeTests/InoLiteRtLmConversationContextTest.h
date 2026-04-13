// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "LiteRtLm/InoLiteRtLmTypes.h"

#include "InoLiteRtLmConversationContextTest.generated.h"

class UInoLiteRtLmSubsystem;

class UInoLiteRtLmConversation;

/**
 * One-shot observer for the Ino.LiteRtLm.ConversationContextTest
 * console command. Exercises the per-turn system/user context injection
 * via SetSystemContext / SetUserContext:
 *
 *   1. Load model via the subsystem (async) -- skipped if already loaded.
 *   2. Create a conversation via the subsystem's factory.
 *   3. Inject system context (location, time_of_day) and user context
 *      (player_name, player_class) into the conversation.
 *   4. Bind OnComplete + OnError to the observer.
 *   5. SendMessageAsync with a prompt that requires the context to answer.
 *   6. On OnComplete, check that the response references the injected
 *      context values and log PASS/FAIL.
 *
 * This validates the full round-trip: SetSystemContext/SetUserContext -->
 * BuildMergedContext --> JSON extra_context --> model sees context -->
 * response references context values.
 *
 * Every handler takes FString by value (not const FString&) to match the
 * dynamic delegate binding convention.
 */
UCLASS()
class UInoLiteRtLmConversationContextTestObserver : public UObject
{
    GENERATED_BODY()

public:
    double StartTime = 0.0;


    TObjectPtr<UInoLiteRtLmSubsystem> Subsystem = nullptr;


    FInoLiteRtLmModelConfig Config;


    TObjectPtr<UInoLiteRtLmConversation> Conversation = nullptr;

    UFUNCTION()
    void HandleModelLoaded(bool bSuccess, FString ErrorMessage);

    UFUNCTION()
    void HandleConversationComplete(FString FullText);

    UFUNCTION()
    void HandleConversationError(FString ErrorMessage);

private:
    void Finish();
};
