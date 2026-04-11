// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// InoAgents.LiteRtLm.ShowChatPanel / HideChatPanel
// ============================================================================
//
// Console commands that put the InoAgents Slate chat panel into and out of
// the active PIE viewport.
//
// Show preconditions:
//   1. PIE is running (so we can find a UGameViewportClient).
//   2. The ULiteRtLmSubsystem has a model loaded — the chat panel does NOT
//      auto-load. Run InoAgents.LiteRtLm.SubsystemLoadTest first.
//
// Show effects:
//   - Tears down any prior live panel (re-entrant safety).
//   - Creates a new ULiteRtLmConversation via the subsystem.
//   - Constructs a UInoAgentsChatBridge (NewObject + AddToRoot).
//   - Constructs the SInoAgentsChatPanel and wraps it in a positioning
//     SBox anchored bottom-right with 24 px padding.
//   - Adds the wrapped panel to the GameViewport via
//     AddViewportWidgetContent at ZOrder 100.
//   - Registers a FEditorDelegates::PrePIEEnded delegate so the panel is
//     torn down BEFORE the GameViewport dies. Without this hook the
//     bridge + conversation leak across PIE sessions and the next
//     subsystem deinit hits a use-after-free in ~SessionBasic.
//
// Hide effects: symmetric — unregisters the PIE-end delegate, calls
// Bridge->Detach(), removes the viewport widget content, drops the
// file-scope smart pointers, and RemoveFromRoot.
//
// Optional: pass `tools` as the first argument to ShowChatPanel to
// auto-register a ULiteRtLmAddNumbersTool with the subsystem before
// creating the conversation. Useful for exercising the tool-call surface
// without typing a separate registration command.
// ============================================================================

#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmAddNumbersTool.h"
#include "LiteRtLm/LiteRtLmConversation.h"
#include "LiteRtLm/LiteRtLmSubsystem.h"
#include "LiteRtLm/LiteRtLmTool.h"
#include "UI/Slate/InoAgentsChatBridge.h"
#include "UI/Slate/SInoAgentsChatPanel.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "HAL/IConsoleManager.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Layout/SBox.h"

#if WITH_EDITOR
    #include "Editor.h"
#endif

namespace
{
    // File-scope state. Single panel at a time — no per-PIE-session
    // bookkeeping needed because Show explicitly tears down any prior
    // panel before creating a new one.
    TStrongObjectPtr<UInoAgentsChatBridge>     GChatBridge;
    TStrongObjectPtr<ULiteRtLmAddNumbersTool>  GAddNumbersTool;
    TSharedPtr<SInoAgentsChatPanel>            GChatPanel;
    TSharedPtr<SWidget>                        GViewportContent;
    TWeakObjectPtr<UGameViewportClient>        GHostViewport;

#if WITH_EDITOR
    FDelegateHandle GPrePIEEndedHandle;
#endif

    ULiteRtLmSubsystem* FindSubsystem()
    {
        if (GEngine == nullptr)
        {
            return nullptr;
        }
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (UGameInstance* GI = Context.OwningGameInstance)
            {
                if (ULiteRtLmSubsystem* Subsys = GI->GetSubsystem<ULiteRtLmSubsystem>())
                {
                    return Subsys;
                }
            }
        }
        return nullptr;
    }

    UGameViewportClient* FindGameViewport()
    {
        if (GEngine == nullptr)
        {
            return nullptr;
        }
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.GameViewport != nullptr)
            {
                return Context.GameViewport;
            }
        }
        return GEngine->GameViewport;
    }

    void RunHideChatPanel()
    {
#if WITH_EDITOR
        if (GPrePIEEndedHandle.IsValid())
        {
            FEditorDelegates::PrePIEEnded.Remove(GPrePIEEndedHandle);
            GPrePIEEndedHandle.Reset();
        }
#endif

        if (GChatBridge.IsValid())
        {
            GChatBridge->Detach();
        }

        if (UGameViewportClient* VC = GHostViewport.Get())
        {
            if (GViewportContent.IsValid())
            {
                VC->RemoveViewportWidgetContent(GViewportContent.ToSharedRef());
            }
        }
        GHostViewport.Reset();
        GViewportContent.Reset();
        GChatPanel.Reset();

        if (GChatBridge.IsValid())
        {
            // TStrongObjectPtr handles RemoveFromRoot/release on Reset.
            GChatBridge.Reset();
        }
        if (GAddNumbersTool.IsValid())
        {
            // Unregister from the subsystem registry if it's still around.
            if (ULiteRtLmSubsystem* Subsys = FindSubsystem())
            {
                Subsys->UnregisterTool(FName(TEXT("add_numbers")));
            }
            GAddNumbersTool.Reset();
        }

        UE_LOG(LogInoAgents, Log, TEXT("ShowChatPanel: hide complete"));
    }

    void RunShowChatPanel(const TArray<FString>& Args)
    {
        // Step 1: any prior panel goes away first. This avoids the
        // single-conversation-enforcement warning log from the subsystem
        // when CreateConversation auto-shuts-down a prior conversation.
        RunHideChatPanel();

        // Step 2: locate the subsystem and the viewport.
        ULiteRtLmSubsystem* Subsys = FindSubsystem();
        if (Subsys == nullptr)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("ShowChatPanel: no ULiteRtLmSubsystem found — start PIE first."));
            return;
        }
        if (!Subsys->IsModelLoaded())
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("ShowChatPanel: no model loaded — run "
                        "InoAgents.LiteRtLm.SubsystemLoadTest (or another loader) first."));
            return;
        }

        UGameViewportClient* VC = FindGameViewport();
        if (VC == nullptr)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("ShowChatPanel: no GameViewport — start PIE first."));
            return;
        }

        // Step 3: optionally register the add_numbers tool so the panel
        // can demonstrate tool calling out of the box.
        const bool bWantTools = Args.Num() > 0
            && Args[0].Equals(TEXT("tools"), ESearchCase::IgnoreCase);
        if (bWantTools)
        {
            ULiteRtLmAddNumbersTool* Tool = NewObject<ULiteRtLmAddNumbersTool>();
            GAddNumbersTool = TStrongObjectPtr<ULiteRtLmAddNumbersTool>(Tool);
            Subsys->RegisterTool(TScriptInterface<ILiteRtLmTool>(Tool));
            UE_LOG(LogInoAgents, Log,
                   TEXT("ShowChatPanel: registered add_numbers tool"));
        }

        // Step 4: create the conversation. Subsystem auto-shuts-down
        // any prior, but RunHideChatPanel above already cleared ours.
        ULiteRtLmConversation* Conv = Subsys->CreateConversation();
        if (Conv == nullptr)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("ShowChatPanel: CreateConversation returned null"));
            if (GAddNumbersTool.IsValid())
            {
                Subsys->UnregisterTool(FName(TEXT("add_numbers")));
                GAddNumbersTool.Reset();
            }
            return;
        }

        // Step 5: build the bridge UObject and pin it.
        UInoAgentsChatBridge* Bridge = NewObject<UInoAgentsChatBridge>();
        GChatBridge = TStrongObjectPtr<UInoAgentsChatBridge>(Bridge);

        // Step 6: build the panel and wire its callbacks to the bridge.
        TSharedRef<SInoAgentsChatPanel> Panel = SNew(SInoAgentsChatPanel)
            .OnMessageSubmitted(FOnInoAgentsChatPanelMessageSubmitted::CreateLambda(
                [](const FString& Text)
                {
                    if (GChatBridge.IsValid())
                    {
                        GChatBridge->SendUserMessage(Text);
                    }
                }))
            .OnCancelRequested(FOnInoAgentsChatPanelCancelRequested::CreateLambda(
                []()
                {
                    if (GChatBridge.IsValid())
                    {
                        GChatBridge->CancelStream();
                    }
                }))
            .OnDismissed(FOnInoAgentsChatPanelDismissed::CreateLambda(
                []()
                {
                    RunHideChatPanel();
                }));

        GChatPanel = Panel;

        // Step 7: position the panel bottom-right with 24 px padding.
        TSharedRef<SWidget> Anchor = SNew(SBox)
            .HAlign(HAlign_Right)
            .VAlign(VAlign_Bottom)
            .Padding(FMargin(0.f, 0.f, 24.f, 24.f))
            [
                Panel
            ];
        GViewportContent = Anchor;
        GHostViewport    = VC;

        VC->AddViewportWidgetContent(Anchor, /*ZOrder=*/100);

        // Step 8: hand the bridge its conversation + panel.
        Bridge->Attach(Panel, Conv);

        // Step 9: focus the input on the next Slate tick.
        Panel->FocusInput();

#if WITH_EDITOR
        // Step 10: register the PIE-end teardown hook. Without this the
        // viewport-widget reference outlives the GameViewport at PIE
        // stop, leaking the bridge + conversation and risking the same
        // ~SessionBasic AV the D.4 fix prevented.
        GPrePIEEndedHandle = FEditorDelegates::PrePIEEnded.AddLambda(
            [](const bool /*bIsSimulating*/)
            {
                RunHideChatPanel();
            });
#endif

        UE_LOG(LogInoAgents, Log,
               TEXT("ShowChatPanel: ready (tools=%s)"),
               bWantTools ? TEXT("on") : TEXT("off"));
    }

    void RunHideChatPanelCommand(const TArray<FString>& /*Args*/)
    {
        RunHideChatPanel();
    }
}

static FAutoConsoleCommand GShowChatPanelCommand(
    TEXT("InoAgents.LiteRtLm.ShowChatPanel"),
    TEXT("Show the in-PIE Slate chat panel for the loaded model. "
         "Requires a model to already be loaded — run "
         "InoAgents.LiteRtLm.SubsystemLoadTest first. "
         "Pass `tools` as the first argument to also auto-register a "
         "ULiteRtLmAddNumbersTool so the panel can exercise tool calling."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunShowChatPanel));

static FAutoConsoleCommand GHideChatPanelCommand(
    TEXT("InoAgents.LiteRtLm.HideChatPanel"),
    TEXT("Tear down the chat panel shown by InoAgents.LiteRtLm.ShowChatPanel. "
         "Symmetric teardown: detaches the bridge, removes the viewport "
         "widget, and shuts down the conversation."),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunHideChatPanelCommand));
