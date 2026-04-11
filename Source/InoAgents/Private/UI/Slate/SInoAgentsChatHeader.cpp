// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "SInoAgentsChatHeader.h"

#include "InoAgentsChatStyle.h"
#include "SInoAgentsStatusDot.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
    // Build a flat icon-style button with no fill at rest, surface fill on
    // hover. Used for both the theme-toggle and the dismiss control.
    FButtonStyle MakeIconButtonStyle(const TSharedPtr<FInoAgentsChatStyle>& Style)
    {
        FButtonStyle Out;
        FSlateBrush Empty;
        Empty.TintColor = FSlateColor(FLinearColor::Transparent);

        Out.SetNormal(Empty);
        Out.SetDisabled(Empty);

        if (Style.IsValid() && Style->IconButtonHoverBrush.IsValid())
        {
            Out.SetHovered(*Style->IconButtonHoverBrush);
            Out.SetPressed(*Style->IconButtonHoverBrush);
        }
        else
        {
            Out.SetHovered(Empty);
            Out.SetPressed(Empty);
        }
        Out.NormalPadding  = FMargin(6.f);
        Out.PressedPadding = FMargin(6.f);
        return Out;
    }
}

void SInoAgentsChatHeader::Construct(const FArguments& InArgs)
{
    Style          = InArgs._Style;
    OnThemeToggled = InArgs._OnThemeToggled;
    OnDismissed    = InArgs._OnDismissed;

    const FSlateFontInfo HeaderFont = Style.IsValid() ? Style->HeaderFont : FSlateFontInfo();
    const FSlateFontInfo IconFont   = Style.IsValid() ? Style->IconFont   : FSlateFontInfo();
    const FLinearColor   TextColour = Style.IsValid() ? Style->TextPrimary : FLinearColor::White;
    const FLinearColor   MutedColour = Style.IsValid() ? Style->TextMuted   : FLinearColor::Gray;

    IconButtonStyle = MakeIconButtonStyle(Style);

    ChildSlot
    .Padding(FMargin(16.f, 12.f, 10.f, 12.f))
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(FMargin(0.f, 0.f, 8.f, 0.f))
        [
            SAssignNew(StatusDot, SInoAgentsStatusDot)
            .Style(Style)
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        [
            SAssignNew(TitleText, STextBlock)
            .Text(FText::FromString(TEXT("InoAgents")))
            .Font(HeaderFont)
            .ColorAndOpacity(FSlateColor(TextColour))
        ]
        + SHorizontalBox::Slot()
        .FillWidth(1.f)
        [
            SNew(SSpacer)
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(FMargin(4.f, 0.f))
        [
            SNew(SButton)
            .ButtonStyle(&IconButtonStyle)
            .ContentPadding(FMargin(6.f, 4.f))
            .OnClicked(this, &SInoAgentsChatHeader::HandleThemeClicked)
            .ToolTipText(FText::FromString(TEXT("Toggle dark/light theme")))
            [
                SAssignNew(ThemeIconText, STextBlock)
                .Text(FText::FromString(Style.IsValid() && Style->bDark ? TEXT("☀") : TEXT("☾")))
                .Font(IconFont)
                .ColorAndOpacity(FSlateColor(MutedColour))
            ]
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(FMargin(4.f, 0.f, 0.f, 0.f))
        [
            SNew(SButton)
            .ButtonStyle(&IconButtonStyle)
            .ContentPadding(FMargin(6.f, 4.f))
            .OnClicked(this, &SInoAgentsChatHeader::HandleDismissClicked)
            .ToolTipText(FText::FromString(TEXT("Close (ESC)")))
            [
                SAssignNew(DismissIconText, STextBlock)
                .Text(FText::FromString(TEXT("✕")))
                .Font(IconFont)
                .ColorAndOpacity(FSlateColor(MutedColour))
            ]
        ]
    ];
}

void SInoAgentsChatHeader::SetStreaming(bool bStreaming)
{
    if (StatusDot.IsValid())
    {
        StatusDot->SetStreaming(bStreaming);
    }
}

void SInoAgentsChatHeader::RefreshStyle(TSharedPtr<FInoAgentsChatStyle> InStyle)
{
    Style = InStyle;

    // Rebuild the icon-button style in place so the buttons pick up the
    // new palette's hover brush on next paint. The pointer the SButtons
    // captured at Construct still points at this member, so the swap is
    // visible to them.
    IconButtonStyle = MakeIconButtonStyle(Style);

    if (StatusDot.IsValid())
    {
        StatusDot->SetStyle(Style);
    }
    if (TitleText.IsValid() && Style.IsValid())
    {
        TitleText->SetFont(Style->HeaderFont);
        TitleText->SetColorAndOpacity(FSlateColor(Style->TextPrimary));
    }
    if (ThemeIconText.IsValid() && Style.IsValid())
    {
        ThemeIconText->SetText(FText::FromString(Style->bDark ? TEXT("☀") : TEXT("☾")));
        ThemeIconText->SetColorAndOpacity(FSlateColor(Style->TextMuted));
    }
    if (DismissIconText.IsValid() && Style.IsValid())
    {
        DismissIconText->SetColorAndOpacity(FSlateColor(Style->TextMuted));
    }
    Invalidate(EInvalidateWidgetReason::Paint | EInvalidateWidgetReason::Layout);
}

FReply SInoAgentsChatHeader::HandleThemeClicked()
{
    OnThemeToggled.ExecuteIfBound();
    return FReply::Handled();
}

FReply SInoAgentsChatHeader::HandleDismissClicked()
{
    OnDismissed.ExecuteIfBound();
    return FReply::Handled();
}
