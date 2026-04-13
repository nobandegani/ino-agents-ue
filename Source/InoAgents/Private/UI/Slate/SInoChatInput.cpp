// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "SInoChatInput.h"

#include "InoChatStyle.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
    // Build a button style backed by the chat style's Send brushes.
    FButtonStyle MakeSendButtonStyle(const TSharedPtr<FInoChatStyle>& Style)
    {
        FButtonStyle Out;
        if (Style.IsValid()
            && Style->SendButtonBrush.IsValid()
            && Style->SendButtonHoverBrush.IsValid())
        {
            Out.SetNormal(*Style->SendButtonBrush);
            Out.SetHovered(*Style->SendButtonHoverBrush);
            Out.SetPressed(*Style->SendButtonHoverBrush);
            Out.SetDisabled(*Style->SendButtonBrush);
        }
        Out.NormalPadding  = FMargin(14.f, 8.f);
        Out.PressedPadding = FMargin(14.f, 8.f);
        return Out;
    }

    FButtonStyle MakeStopButtonStyle(const TSharedPtr<FInoChatStyle>& Style)
    {
        FButtonStyle Out;
        if (Style.IsValid()
            && Style->StopButtonBrush.IsValid()
            && Style->StopButtonHoverBrush.IsValid())
        {
            Out.SetNormal(*Style->StopButtonBrush);
            Out.SetHovered(*Style->StopButtonHoverBrush);
            Out.SetPressed(*Style->StopButtonHoverBrush);
            Out.SetDisabled(*Style->StopButtonBrush);
        }
        Out.NormalPadding  = FMargin(14.f, 8.f);
        Out.PressedPadding = FMargin(14.f, 8.f);
        return Out;
    }
}

void SInoChatInput::Construct(const FArguments& InArgs)
{
    Style             = InArgs._Style;
    OnSubmitted       = InArgs._OnSubmitted;
    OnCancelRequested = InArgs._OnCancelRequested;

    const FSlateFontInfo BodyFont   = Style.IsValid() ? Style->BodyFont    : FSlateFontInfo();
    const FLinearColor   TextColour = Style.IsValid() ? Style->TextPrimary : FLinearColor::White;
    const FLinearColor   MutedColour= Style.IsValid() ? Style->TextMuted   : FLinearColor::Gray;
    const FSlateBrush*   InputBg    = (Style.IsValid() && Style->InputBrush.IsValid())
        ? Style->InputBrush.Get() : FCoreStyle::Get().GetBrush("WhiteBrush");

    // Build a custom editable-text-box style that matches our palette.
    // Stored as a member because the SMultiLineEditableTextBox reads the
    // pointer at Construct time and may keep referring to it during paint.
    CustomTextBoxStyle = FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
    if (Style.IsValid())
    {
        CustomTextBoxStyle.BackgroundImageNormal   = FSlateNoResource();
        CustomTextBoxStyle.BackgroundImageHovered  = FSlateNoResource();
        CustomTextBoxStyle.BackgroundImageFocused  = FSlateNoResource();
        CustomTextBoxStyle.BackgroundImageReadOnly = FSlateNoResource();
        CustomTextBoxStyle.ForegroundColor         = FSlateColor(Style->TextPrimary);
        CustomTextBoxStyle.BackgroundColor         = FSlateColor(FLinearColor::Transparent);
        CustomTextBoxStyle.SetFont(Style->BodyFont);
    }

    ChildSlot
    .Padding(FMargin(14.f, 4.f, 14.f, 14.f))
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot()
        .FillWidth(1.f)
        .Padding(FMargin(0.f, 0.f, 8.f, 0.f))
        [
            SAssignNew(InputBorder, SBorder)
            .BorderImage(InputBg)
            .Padding(FMargin(10.f, 6.f))
            [
                SNew(SBox)
                .MinDesiredHeight(36.f)
                .MaxDesiredHeight(120.f)
                [
                    SAssignNew(TextBox, SMultiLineEditableTextBox)
                    .Style(&CustomTextBoxStyle)
                    .Font(BodyFont)
                    .ForegroundColor(FSlateColor(TextColour))
                    .HintText(FText::FromString(TEXT("Type a message…")))
                    .AllowMultiLine(true)
                    .AlwaysShowScrollbars(false)
                    .OnKeyDownHandler(this, &SInoChatInput::HandleKeyDownInTextBox)
                ]
            ]
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Bottom)
        [
            SAssignNew(ActionButton, SButton)
            .ContentPadding(FMargin(14.f, 8.f))
            .OnClicked(this, &SInoChatInput::HandleSendClicked)
            [
                SAssignNew(ActionLabel, STextBlock)
                .Text(FText::FromString(TEXT("Send")))
                .Font(BodyFont)
                .ColorAndOpacity(FSlateColor(Style.IsValid() ? Style->TextOnAccent : FLinearColor::White))
            ]
        ]
    ];

    RebuildButton();
}

void SInoChatInput::RebuildButton()
{
    if (!ActionButton.IsValid() || !Style.IsValid())
    {
        return;
    }

    // Refresh the per-instance button styles, then point the button at
    // the right one for the current streaming state.
    SendButtonStyle = MakeSendButtonStyle(Style);
    StopButtonStyle = MakeStopButtonStyle(Style);

    ActionButton->SetButtonStyle(bStreaming ? &StopButtonStyle : &SendButtonStyle);

    if (ActionLabel.IsValid())
    {
        ActionLabel->SetText(FText::FromString(bStreaming ? TEXT("Stop") : TEXT("Send")));
        ActionLabel->SetColorAndOpacity(FSlateColor(Style->TextOnAccent));
        ActionLabel->SetFont(Style->BodyFont);
    }
}

void SInoChatInput::SetStreaming(bool bInStreaming)
{
    if (bStreaming == bInStreaming)
    {
        return;
    }
    bStreaming = bInStreaming;

    if (TextBox.IsValid())
    {
        TextBox->SetIsReadOnly(bStreaming);
    }
    RebuildButton();
}

void SInoChatInput::RefreshStyle(TSharedPtr<FInoChatStyle> InStyle)
{
    Style = InStyle;

    // Rebind the input row's outer border to the new style's brush so
    // it actually picks up the new theme's colours. The brush memory
    // from the prior style is still valid because the panel pins both
    // palettes for its lifetime, but we still need to repoint the
    // SBorder to read from the new one.
    if (InputBorder.IsValid() && Style.IsValid() && Style->InputBrush.IsValid())
    {
        InputBorder->SetBorderImage(Style->InputBrush.Get());
    }

    // Refresh the editable text-box style colours so foreground/font
    // pick up the new theme. The pointer the SMultiLineEditableTextBox
    // captured at Construct is to our member, so the in-place edit is
    // visible to it.
    if (Style.IsValid())
    {
        CustomTextBoxStyle.ForegroundColor = FSlateColor(Style->TextPrimary);
        CustomTextBoxStyle.SetFont(Style->BodyFont);
    }

    RebuildButton();
    Invalidate(EInvalidateWidgetReason::Paint | EInvalidateWidgetReason::Layout);
}

void SInoChatInput::FocusTextBox()
{
    if (TextBox.IsValid())
    {
        FSlateApplication::Get().SetUserFocus(0, TextBox, EFocusCause::SetDirectly);
    }
}

FReply SInoChatInput::HandleSendClicked()
{
    if (bStreaming)
    {
        OnCancelRequested.ExecuteIfBound();
        return FReply::Handled();
    }
    SubmitCurrentText();
    return FReply::Handled();
}

FReply SInoChatInput::HandleKeyDownInTextBox(
    const FGeometry& MyGeometry, const FKeyEvent& KeyEvent)
{
    // Enter (no Shift) submits. Shift+Enter falls through to default,
    // which inserts a newline like every multiline box does.
    if (KeyEvent.GetKey() == EKeys::Enter && !KeyEvent.IsShiftDown())
    {
        if (!bStreaming)
        {
            SubmitCurrentText();
        }
        return FReply::Handled();
    }
    return FReply::Unhandled();
}

void SInoChatInput::SubmitCurrentText()
{
    if (!TextBox.IsValid())
    {
        return;
    }
    const FString Text = TextBox->GetText().ToString().TrimStartAndEnd();
    if (Text.IsEmpty())
    {
        return;
    }
    OnSubmitted.ExecuteIfBound(Text);
    TextBox->SetText(FText::GetEmpty());
}
