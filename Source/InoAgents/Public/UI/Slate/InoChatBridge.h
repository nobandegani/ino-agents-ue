// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "InoChatBridge.generated.h"

class UInoLiteRtLmConversation;
class SInoChatPanel;

/**
 * Glue between a Slate SInoChatPanel and a UInoLiteRtLmConversation.
 *
 * Necessary because the conversation's delegates
 * (FOnInoLiteRtLmToken/Complete/Error/ToolCalled) are
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
 * exemplar at InoLiteRtLmConversationToolTest.cpp:126.
 */
UCLASS()
class INOAGENTS_API UInoChatBridge : public UObject
{
    GENERATED_BODY()

public:
    /** Bind to a conversation and a panel. The user-message-submitted
     *  callback on the panel is wired to SendUserMessage. */
    void Attach(TSharedRef<SInoChatPanel> InPanel, UInoLiteRtLmConversation* InConversation);

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
    TObjectPtr<UInoLiteRtLmConversation> Conversation = nullptr;

    // Weak so the bridge does not keep the Slate widget alive — the
    // viewport widget container owns that decision.
    TWeakPtr<SInoChatPanel> PanelWeak;
};
