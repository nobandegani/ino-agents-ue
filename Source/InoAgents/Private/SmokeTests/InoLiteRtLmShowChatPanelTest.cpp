// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.LiteRtLm.ShowChatPanel / HideChatPanel
// ============================================================================
//
// Console commands that delegate to
// UInoLiteRtLmSubsystem::ShowChatPanel / HideChatPanel. The subsystem owns
// the actual panel lifecycle so the same API is available from both
// console commands and Blueprint.
//
// Optional: pass `tools` as the first argument to ShowChatPanel to
// auto-register a UInoLiteRtLmAddNumbersTool with the subsystem before
// creating the conversation. Useful for exercising the tool-call surface
// without typing a separate registration command.
// ============================================================================

#include "InoAgentsLog.h"
#include "LiteRtLm/InoLiteRtLmAddNumbersTool.h"
#include "LiteRtLm/InoLiteRtLmSubsystem.h"
#include "LiteRtLm/InoLiteRtLmToolBase.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    // Kept alive for the duration of the panel so GC doesn't eat it.
    // Reset in the Hide command.
    TStrongObjectPtr<UInoLiteRtLmAddNumbersTool> GAddNumbersTool;

    UInoLiteRtLmSubsystem* FindSubsystem()
    {
        if (GEngine == nullptr)
        {
            return nullptr;
        }
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (UGameInstance* GI = Context.OwningGameInstance)
            {
                if (UInoLiteRtLmSubsystem* Subsys = GI->GetSubsystem<UInoLiteRtLmSubsystem>())
                {
                    return Subsys;
                }
            }
        }
        return nullptr;
    }

    void RunShowChatPanel(const TArray<FString>& Args)
    {
        UInoLiteRtLmSubsystem* Subsys = FindSubsystem();
        if (Subsys == nullptr)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("ShowChatPanel: no UInoLiteRtLmSubsystem found — start PIE first."));
            return;
        }

        // Optionally register the add_numbers tool so the panel can
        // demonstrate tool calling out of the box.
        const bool bWantTools = Args.Num() > 0
            && Args[0].Equals(TEXT("tools"), ESearchCase::IgnoreCase);
        if (bWantTools)
        {
            UInoLiteRtLmAddNumbersTool* Tool = NewObject<UInoLiteRtLmAddNumbersTool>();
            GAddNumbersTool = TStrongObjectPtr<UInoLiteRtLmAddNumbersTool>(Tool);
            Subsys->RegisterTool(Tool);
            UE_LOG(LogInoAgents, Log,
                   TEXT("ShowChatPanel: registered add_numbers tool"));
        }

        // Delegate to the subsystem — it handles everything from here:
        // tearing down any prior panel, locating the viewport, creating
        // a conversation (if null is passed), building the bridge+panel,
        // and registering the PIE-end teardown hook.
        Subsys->ShowChatPanel(/*Conversation=*/nullptr);
    }

    void RunHideChatPanel(const TArray<FString>& /*Args*/)
    {
        UInoLiteRtLmSubsystem* Subsys = FindSubsystem();
        if (Subsys != nullptr)
        {
            Subsys->HideChatPanel();
        }

        // Unregister the tool if we registered one.
        if (GAddNumbersTool.IsValid())
        {
            if (Subsys != nullptr)
            {
                Subsys->UnregisterTool(FName(TEXT("add_numbers")));
            }
            GAddNumbersTool.Reset();
        }
    }
}

static FAutoConsoleCommand GShowChatPanelCommand(
    TEXT("Ino.LiteRtLm.ShowChatPanel"),
    TEXT("Show the in-PIE Slate chat panel for the loaded model. "
         "Requires a model to already be loaded — run "
         "Ino.LiteRtLm.SubsystemLoadTest first. "
         "Pass `tools` as the first argument to also auto-register a "
         "UInoLiteRtLmAddNumbersTool so the panel can exercise tool calling."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunShowChatPanel));

static FAutoConsoleCommand GHideChatPanelCommand(
    TEXT("Ino.LiteRtLm.HideChatPanel"),
    TEXT("Tear down the chat panel shown by Ino.LiteRtLm.ShowChatPanel. "
         "Symmetric teardown: detaches the bridge, removes the viewport "
         "widget, and shuts down the conversation."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunHideChatPanel));
