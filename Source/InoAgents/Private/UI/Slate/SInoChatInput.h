// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

struct FInoChatStyle;
class SMultiLineEditableTextBox;
class SButton;
class STextBlock;
class SBorder;

DECLARE_DELEGATE_OneParam(FOnInoChatMessageSubmitted, const FString&);
DECLARE_DELEGATE(FOnInoChatCancelRequested);

/**
 * Bottom input row of the chat panel: a multiline editable text box on
 * the left, a Send/Stop button on the right.
 *
 * Enter (without Shift) submits the current text. Shift+Enter inserts a
 * newline. Empty input + Enter is a no-op.
 *
 * While streaming, the Send button swaps to a Stop button (different
 * background brush + label) that fires OnCancelRequested. The input box
 * is also disabled while streaming so users can't queue messages mid-
 * generation; the conversation API supports queuing but for a debug
 * surface keeping the UX strict is clearer.
 */
class SInoChatInput : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SInoChatInput) {}
        SLATE_ARGUMENT(TSharedPtr<FInoChatStyle>, Style)
        SLATE_EVENT(FOnInoChatMessageSubmitted, OnSubmitted)
        SLATE_EVENT(FOnInoChatCancelRequested,  OnCancelRequested)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Toggle Send↔Stop. Called by the panel from SetStreaming. */
    void SetStreaming(bool bStreaming);

    /** Re-resolve colours/brushes/fonts after a theme toggle. */
    void RefreshStyle(TSharedPtr<FInoChatStyle> InStyle);

    /** Move keyboard focus into the text box. Called via active timer
     *  from the panel's Construct and after every OnComplete. */
    void FocusTextBox();

    bool IsStreaming() const { return bStreaming; }

private:
    FReply HandleSendClicked();
    FReply HandleKeyDownInTextBox(const FGeometry&, const FKeyEvent&);
    void   SubmitCurrentText();
    void   RebuildButton();

    TSharedPtr<FInoChatStyle> Style;
    FOnInoChatMessageSubmitted OnSubmitted;
    FOnInoChatCancelRequested  OnCancelRequested;

    TSharedPtr<SMultiLineEditableTextBox> TextBox;
    TSharedPtr<SBorder>    InputBorder;  // outer rounded border, repointed on theme toggle
    TSharedPtr<SButton>    ActionButton;
    TSharedPtr<STextBlock> ActionLabel;

    // Stored by value because Slate's editable text box reads its style
    // pointer from the FArguments and we need it to outlive Construct.
    FEditableTextBoxStyle CustomTextBoxStyle;
    FButtonStyle SendButtonStyle;
    FButtonStyle StopButtonStyle;

    bool bStreaming = false;
};
