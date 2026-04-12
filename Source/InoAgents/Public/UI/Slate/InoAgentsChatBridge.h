// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "InoAgentsChatBridge.generated.h"

class ULiteRtLmConversation;
class SInoAgentsChatPanel;

/**
 * Glue between a Slate SInoAgentsChatPanel and a ULiteRtLmConversation.
 *
 * Necessary because the conversation's delegates
 * (FOnLiteRtLmToken/Complete/Error/ToolCalled) are
 * DECLARE_DYNAMIC_MULTICAST_DELEGATE_*, which can only be bound to
 * UFUNCTION methods on a UObject. A pure SWidget cannot bind. The
 * bridge holds the conversation strong reference, exposes UFUNCTION
 * handlers, and forwards events into the panel through a TWeakPtr.
 *
 * Lifecycle: NewObject + AddToRoot in the console command, Detach +
 * RemoveFromRoot in the Hide path. Holds the conversation alive
 * across GC sweeps via the UPROPERTY strong ref.
 *
 * All handlers take FString / FName by VALUE (not const&) — this is
 * mandatory for AddDynamic to match the delegate signatures, see the
 * exemplar at InoAgentsLiteRtLmConversationToolTest.cpp:126.
 */
UCLASS()
class INOAGENTS_API UInoAgentsChatBridge : public UObject
{
    GENERATED_BODY()

public:
    /** Bind to a conversation and a panel. The user-message-submitted
     *  callback on the panel is wired to SendUserMessage. */
    void Attach(TSharedRef<SInoAgentsChatPanel> InPanel, ULiteRtLmConversation* InConversation);

    /** Symmetric teardown: unbind delegates, shut down the conversation
     *  if still valid, clear the weak panel ref. Idempotent. */
    void Detach();

    /** Forward a user-typed message to the conversation and update the
     *  panel's streaming state. Called by the panel's
     *  OnMessageSubmitted SLATE_EVENT after the bridge has been attached. */
    void SendUserMessage(const FString& Text);

    /** Cancel an in-flight stream. Called by the panel's
     *  OnCancelRequested SLATE_EVENT. */
    void CancelStream();

    UFUNCTION()
    void HandleToken(FString RawText, FString CleanText);

    UFUNCTION()
    void HandleComplete(FString FullText);

    UFUNCTION()
    void HandleError(FString ErrorMessage);

    UFUNCTION()
    void HandleToolCalled(FName ToolName, FString ArgumentsJson, FString ResultJson);

private:
    UPROPERTY()
    TObjectPtr<ULiteRtLmConversation> Conversation = nullptr;

    // Weak so the bridge does not keep the Slate widget alive — the
    // viewport widget container owns that decision.
    TWeakPtr<SInoAgentsChatPanel> PanelWeak;
};
