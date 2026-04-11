// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Animation/CurveSequence.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

struct FInoAgentsChatStyle;
class SInoAgentsChatHeader;
class SInoAgentsChatInput;
class SInoAgentsMessageBubble;
class SInoAgentsToolPill;
class SVerticalBox;
class SScrollBox;

// Forward-declared so the panel's public header doesn't need to include
// the private SInoAgentsMessageBubble.h. C++11 enum-class forward decls
// require the underlying type to be specified — keep this in sync with
// the definition in SInoAgentsMessageBubble.h.
enum class EInoAgentsBubbleRole : uint8;

DECLARE_DELEGATE_OneParam(FOnInoAgentsChatPanelMessageSubmitted, const FString&);
DECLARE_DELEGATE(FOnInoAgentsChatPanelDismissed);
DECLARE_DELEGATE(FOnInoAgentsChatPanelCancelRequested);

/**
 * Top-level Slate widget for the InoAgents chat panel.
 *
 * Drives the entire visual surface and exposes a small mutating API that
 * the UInoAgentsChatBridge UObject calls to push tokens, tool calls, and
 * terminal events from a ULiteRtLmConversation.
 *
 * The panel does NOT own the conversation. It is a passive renderer that
 * receives state changes via PushUserMessage, AppendAssistantToken,
 * FinaliseAssistantMessage, PushToolCall, and SetError, and reports user
 * actions via SLATE_EVENT delegates.
 *
 * Threading: every method on this class must be called from the game
 * thread. The bridge already enforces this because the conversation's
 * own delegates fire on the game thread.
 */
class INOAGENTS_API SInoAgentsChatPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SInoAgentsChatPanel) {}
        SLATE_EVENT(FOnInoAgentsChatPanelMessageSubmitted, OnMessageSubmitted)
        SLATE_EVENT(FOnInoAgentsChatPanelDismissed,        OnDismissed)
        SLATE_EVENT(FOnInoAgentsChatPanelCancelRequested,  OnCancelRequested)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    // ----- mutators called by the bridge -----

    /** Add a user-authored message to the message list. */
    void PushUserMessage(const FText& Text);

    /** Append a streaming chunk to the trailing assistant bubble.
     *  Lazily creates the bubble on the first chunk after either a
     *  user message or a tool call so the message stream stays clean. */
    void AppendAssistantToken(const FString& Chunk);

    /** Signal that the current assistant turn is complete. Does NOT
     *  replace the bubble's text — see plan: OnToken-concatenated text
     *  is byte-equal to OnComplete's FullText. */
    void FinaliseAssistantMessage();

    /** Add a tool-call display between assistant text segments. */
    void PushToolCall(FName ToolName, const FString& ArgsJson, const FString& ResultJson);

    /** Replace the trailing message with an error bubble (or append a
     *  new one if the trailing message is not assistant-in-progress). */
    void SetError(const FText& Message);

    /** Drives the status dot, the input read-only state, and the
     *  Send↔Stop button swap. */
    void SetStreaming(bool bStreaming);

    /** Hot-swap the entire chat style atomically. Bubbles and pills
     *  receive RefreshStyle() callbacks; nothing is rebuilt. */
    void SetTheme(bool bDark);

    /** Move keyboard focus into the text input. */
    void FocusInput();

private:
    bool SupportsKeyboardFocus() const override { return true; }
    FReply OnKeyDown(const FGeometry&, const FKeyEvent&) override;
    void   Tick(const FGeometry&, const double, const float) override;
    int32  OnPaint(
        const FPaintArgs&,
        const FGeometry&,
        const FSlateRect&,
        FSlateWindowElementList&,
        int32,
        const FWidgetStyle&,
        bool) const override;

    void HandleMessageSubmittedFromInput(const FString& Text);
    void HandleCancelFromInput();
    void HandleDismissFromHeader();
    void HandleThemeToggleFromHeader();
    void HandleUserScrolled(float InOffsetFraction);

    TSharedRef<SInoAgentsMessageBubble> MakeBubble(
        EInoAgentsBubbleRole Role, const FText& InitialText);

    void AppendBubbleToList(TSharedRef<SWidget> Bubble);
    void RefreshAllChildStyles();
    void MaybeAutoScrollToBottom();

    TSharedPtr<FInoAgentsChatStyle> Style;

    FOnInoAgentsChatPanelMessageSubmitted OnMessageSubmitted;
    FOnInoAgentsChatPanelDismissed        OnDismissed;
    FOnInoAgentsChatPanelCancelRequested  OnCancelRequested;

    TSharedPtr<SInoAgentsChatHeader> Header;
    TSharedPtr<SInoAgentsChatInput>  Input;
    TSharedPtr<SScrollBox>           MessageScrollBox;
    TSharedPtr<SVerticalBox>         MessageList;

    // Parallel arrays for in-place style refresh.
    TArray<TSharedPtr<SInoAgentsMessageBubble>> BubbleWidgets;
    TArray<TSharedPtr<SInoAgentsToolPill>>      PillWidgets;

    // The bubble that's currently receiving streaming tokens, if any.
    // Reset to nullptr by FinaliseAssistantMessage / PushToolCall /
    // PushUserMessage so the next AppendAssistantToken creates a fresh
    // bubble.
    TWeakPtr<SInoAgentsMessageBubble> ActiveAssistantBubble;

    bool bStreaming = false;
    bool bUserScrolledUp = false;

    FCurveSequence EnterSequence;
};
