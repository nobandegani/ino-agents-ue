// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "LiteRtLm/LiteRtLmTypes.h"

#include "InoAgentsLiteRtLmConversationStreamTest.generated.h"

class ULiteRtLmSubsystem;

class ULiteRtLmConversation;

/**
 * One-shot observer for the InoAgents.LiteRtLm.ConversationStreamTest
 * console command. Orchestrates:
 *   1. Load model via the subsystem (async) — skipped if already loaded.
 *   2. Create a conversation via the subsystem's factory.
 *   3. Bind OnToken + OnComplete + OnError to the observer's UFUNCTIONs.
 *   4. SendMessageAsync with the test prompt.
 *   5. As each chunk arrives, log it with a timestamp relative to stream
 *      start.
 *   6. On OnComplete, log chunk count, elapsed time, effective chunks/sec,
 *      and the full accumulated text. Release the observer.
 *   7. On OnError, log the error and release the observer.
 *
 * This is the first test that exercises the streaming path; previous
 * D.2 test (ConversationSendTest) only bound OnComplete/OnError and
 * did not listen to OnToken.
 *
 * Holds UPROPERTY references to the subsystem, config, and conversation
 * so none of them get garbage-collected mid-stream. AddToRoot keeps the
 * observer itself alive. RemoveFromRoot in the final handler releases
 * everything.
 *
 * Every handler takes FString by value (not const FString&) to match the
 * dynamic delegate binding convention documented in the D.1 / D.2 observers.
 */
UCLASS()
class UInoAgentsLiteRtLmConversationStreamTestObserver : public UObject
{
    GENERATED_BODY()

public:
    /** Wall-clock time captured when the console command was issued. */
    double StartTime = 0.0;

    /** Wall-clock time captured when the first OnToken fires. */
    double FirstTokenTime = 0.0;

    /** Accumulated token count (one per OnToken broadcast). */
    int32 NumTokens = 0;

    /** Locally-accumulated text, built up inside HandleToken. Used to
     *  cross-check against the FullText delivered to HandleComplete. */
    FString AccumulatedText;

    FString Prompt;

    
    TObjectPtr<ULiteRtLmSubsystem> Subsystem = nullptr;

    
    FLiteRtLmModelConfig Config;

    
    TObjectPtr<ULiteRtLmConversation> Conversation = nullptr;

    // Fires after LoadModelAsync (or directly if the model is already
    // loaded).
    UFUNCTION()
    void HandleModelLoaded(bool bSuccess, FString ErrorMessage);

    // Fires for every streamed chunk on the game thread.
    UFUNCTION()
    void HandleToken(FString Chunk);

    // Fires once on clean stream completion.
    UFUNCTION()
    void HandleConversationComplete(FString FullText);

    // Fires once on failure or cancellation.
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
     * otherwise produce (same rationale as the D.2 observer).
     */
    void Finish();
};
