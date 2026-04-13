// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "UI/Slate/InoChatBridge.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/InoLiteRtLmConversation.h"
#include "UI/Slate/SInoChatPanel.h"

void UInoChatBridge::Attach(
    TSharedRef<SInoChatPanel> InPanel, UInoLiteRtLmConversation* InConversation)
{
    Conversation = InConversation;
    PanelWeak    = InPanel;

    if (Conversation == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("ChatBridge::Attach: conversation is null"));
        return;
    }

    Conversation->OnToken.AddDynamic(this,       &UInoChatBridge::HandleToken);
    Conversation->OnComplete.AddDynamic(this,    &UInoChatBridge::HandleComplete);
    Conversation->OnError.AddDynamic(this,       &UInoChatBridge::HandleError);
    Conversation->OnToolCalled.AddDynamic(this,  &UInoChatBridge::HandleToolCalled);

    UE_LOG(LogInoAgents, Log, TEXT("ChatBridge: attached to conversation"));
}

void UInoChatBridge::Detach()
{
    // The subsystem may have already shut down the conversation
    // (single-conversation enforcement, model unload, PIE end). Check
    // IsValid before touching it.
    if (Conversation != nullptr && IsValid(Conversation))
    {
        Conversation->OnToken.RemoveDynamic(this,      &UInoChatBridge::HandleToken);
        Conversation->OnComplete.RemoveDynamic(this,   &UInoChatBridge::HandleComplete);
        Conversation->OnError.RemoveDynamic(this,      &UInoChatBridge::HandleError);
        Conversation->OnToolCalled.RemoveDynamic(this, &UInoChatBridge::HandleToolCalled);

        Conversation->Shutdown();
    }
    Conversation = nullptr;
    PanelWeak.Reset();
    UE_LOG(LogInoAgents, Log, TEXT("ChatBridge: detached"));
}

void UInoChatBridge::SendUserMessage(const FString& Text)
{
    if (Conversation == nullptr || !IsValid(Conversation))
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("ChatBridge::SendUserMessage: conversation is gone, dropping"));
        return;
    }

    if (TSharedPtr<SInoChatPanel> Panel = PanelWeak.Pin())
    {
        Panel->SetStreaming(true);
    }

    Conversation->SendMessageAsync(Text);
}

void UInoChatBridge::CancelStream()
{
    if (Conversation != nullptr && IsValid(Conversation))
    {
        Conversation->Cancel();
    }
}

void UInoChatBridge::HandleToken(FString RawText, FString CleanText)
{
    if (TSharedPtr<SInoChatPanel> Panel = PanelWeak.Pin())
    {
        Panel->AppendAssistantToken(CleanText);
    }
}

void UInoChatBridge::HandleComplete(FString FullText)
{
    if (TSharedPtr<SInoChatPanel> Panel = PanelWeak.Pin())
    {
        Panel->FinaliseAssistantMessage();
        Panel->SetStreaming(false);
        Panel->FocusInput();
    }
}

void UInoChatBridge::HandleError(FString ErrorMessage)
{
    if (TSharedPtr<SInoChatPanel> Panel = PanelWeak.Pin())
    {
        Panel->SetError(FText::FromString(FString::Printf(
            TEXT("Error: %s"), *ErrorMessage)));
        Panel->SetStreaming(false);
        Panel->FocusInput();
    }
}

void UInoChatBridge::HandleToolCalled(
    FName ToolName, FString ArgumentsJson, FString ResultJson)
{
    if (TSharedPtr<SInoChatPanel> Panel = PanelWeak.Pin())
    {
        Panel->PushToolCall(ToolName, ArgumentsJson, ResultJson);
    }
}
