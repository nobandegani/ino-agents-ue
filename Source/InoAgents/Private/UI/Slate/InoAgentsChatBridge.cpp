// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "UI/Slate/InoAgentsChatBridge.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmConversation.h"
#include "UI/Slate/SInoAgentsChatPanel.h"

void UInoAgentsChatBridge::Attach(
    TSharedRef<SInoAgentsChatPanel> InPanel, ULiteRtLmConversation* InConversation)
{
    Conversation = InConversation;
    PanelWeak    = InPanel;

    if (Conversation == nullptr)
    {
        UE_LOG(LogInoAgents, Error, TEXT("ChatBridge::Attach: conversation is null"));
        return;
    }

    Conversation->OnToken.AddDynamic(this,       &UInoAgentsChatBridge::HandleToken);
    Conversation->OnComplete.AddDynamic(this,    &UInoAgentsChatBridge::HandleComplete);
    Conversation->OnError.AddDynamic(this,       &UInoAgentsChatBridge::HandleError);
    Conversation->OnToolCalled.AddDynamic(this,  &UInoAgentsChatBridge::HandleToolCalled);

    UE_LOG(LogInoAgents, Log, TEXT("ChatBridge: attached to conversation"));
}

void UInoAgentsChatBridge::Detach()
{
    // The subsystem may have already shut down the conversation
    // (single-conversation enforcement, model unload, PIE end). Check
    // IsValid before touching it.
    if (Conversation != nullptr && IsValid(Conversation))
    {
        Conversation->OnToken.RemoveDynamic(this,      &UInoAgentsChatBridge::HandleToken);
        Conversation->OnComplete.RemoveDynamic(this,   &UInoAgentsChatBridge::HandleComplete);
        Conversation->OnError.RemoveDynamic(this,      &UInoAgentsChatBridge::HandleError);
        Conversation->OnToolCalled.RemoveDynamic(this, &UInoAgentsChatBridge::HandleToolCalled);

        Conversation->Shutdown();
    }
    Conversation = nullptr;
    PanelWeak.Reset();
    UE_LOG(LogInoAgents, Log, TEXT("ChatBridge: detached"));
}

void UInoAgentsChatBridge::SendUserMessage(const FString& Text)
{
    if (Conversation == nullptr || !IsValid(Conversation))
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("ChatBridge::SendUserMessage: conversation is gone, dropping"));
        return;
    }

    if (TSharedPtr<SInoAgentsChatPanel> Panel = PanelWeak.Pin())
    {
        Panel->SetStreaming(true);
    }

    Conversation->SendMessageAsync(Text);
}

void UInoAgentsChatBridge::CancelStream()
{
    if (Conversation != nullptr && IsValid(Conversation))
    {
        Conversation->Cancel();
    }
}

void UInoAgentsChatBridge::HandleToken(FString RawText, FString CleanText)
{
    if (TSharedPtr<SInoAgentsChatPanel> Panel = PanelWeak.Pin())
    {
        Panel->AppendAssistantToken(CleanText);
    }
}

void UInoAgentsChatBridge::HandleComplete(FString FullText)
{
    if (TSharedPtr<SInoAgentsChatPanel> Panel = PanelWeak.Pin())
    {
        Panel->FinaliseAssistantMessage();
        Panel->SetStreaming(false);
        Panel->FocusInput();
    }
}

void UInoAgentsChatBridge::HandleError(FString ErrorMessage)
{
    if (TSharedPtr<SInoAgentsChatPanel> Panel = PanelWeak.Pin())
    {
        Panel->SetError(FText::FromString(FString::Printf(
            TEXT("Error: %s"), *ErrorMessage)));
        Panel->SetStreaming(false);
        Panel->FocusInput();
    }
}

void UInoAgentsChatBridge::HandleToolCalled(
    FName ToolName, FString ArgumentsJson, FString ResultJson)
{
    if (TSharedPtr<SInoAgentsChatPanel> Panel = PanelWeak.Pin())
    {
        Panel->PushToolCall(ToolName, ArgumentsJson, ResultJson);
    }
}
