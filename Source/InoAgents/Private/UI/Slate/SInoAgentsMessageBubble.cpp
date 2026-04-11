// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "SInoAgentsMessageBubble.h"

#include "InoAgentsChatStyle.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SInoAgentsMessageBubble::Construct(const FArguments& InArgs)
{
    Style       = InArgs._Style;
    Role        = InArgs._Role;
    CurrentText = InArgs._InitialText;

    EnterSequence = FCurveSequence();
    EnterSequence.AddCurve(0.f, 0.18f, ECurveEaseFunction::CubicOut);
    EnterSequence.Play(SharedThis(this));
    SetCanTick(true);

    RebuildContent();
}

void SInoAgentsMessageBubble::RebuildContent()
{
    if (!Style.IsValid())
    {
        return;
    }

    const FSlateBrush* BubbleBg = nullptr;
    FLinearColor TextColour = Style->TextPrimary;
    EHorizontalAlignment HAlign = HAlign_Left;

    switch (Role)
    {
    case EInoAgentsBubbleRole::User:
        BubbleBg = Style->UserBubbleBrush.IsValid()
            ? Style->UserBubbleBrush.Get()
            : FCoreStyle::Get().GetBrush("WhiteBrush");
        HAlign = HAlign_Right;
        TextColour = Style->TextPrimary;
        break;

    case EInoAgentsBubbleRole::Assistant:
        BubbleBg = Style->AssistantBubbleBrush.IsValid()
            ? Style->AssistantBubbleBrush.Get()
            : FCoreStyle::Get().GetBrush("WhiteBrush");
        HAlign = HAlign_Left;
        TextColour = Style->TextPrimary;
        break;

    case EInoAgentsBubbleRole::Error:
        BubbleBg = Style->AssistantBubbleBrush.IsValid()
            ? Style->AssistantBubbleBrush.Get()
            : FCoreStyle::Get().GetBrush("WhiteBrush");
        HAlign = HAlign_Left;
        TextColour = Style->Error;
        break;
    }

    ChildSlot
    .HAlign(HAlign)
    .Padding(FMargin(0.f, 4.f))
    [
        // Cap the bubble width at ~78% of typical panel width so very
        // long replies wrap rather than ballooning the panel.
        SNew(SBox)
        .MaxDesiredWidth(380.f)
        [
            SNew(SBorder)
            .BorderImage(BubbleBg)
            .Padding(FMargin(12.f, 9.f))
            [
                SAssignNew(TextBlock, STextBlock)
                .Text(CurrentText)
                .Font(Style->BodyFont)
                .ColorAndOpacity(FSlateColor(TextColour))
                .AutoWrapText(true)
            ]
        ]
    ];
}

void SInoAgentsMessageBubble::SetText(const FText& InText)
{
    CurrentText = InText;
    if (TextBlock.IsValid())
    {
        TextBlock->SetText(CurrentText);
    }
}

void SInoAgentsMessageBubble::AppendText(const FString& Chunk)
{
    if (Chunk.IsEmpty())
    {
        return;
    }
    CurrentText = FText::FromString(CurrentText.ToString() + Chunk);
    if (TextBlock.IsValid())
    {
        TextBlock->SetText(CurrentText);
    }
}

FText SInoAgentsMessageBubble::GetText() const
{
    return CurrentText;
}

void SInoAgentsMessageBubble::RefreshStyle(TSharedPtr<FInoAgentsChatStyle> InStyle)
{
    Style = InStyle;
    RebuildContent();
}

void SInoAgentsMessageBubble::Tick(const FGeometry&, const double, const float)
{
    if (EnterSequence.IsPlaying())
    {
        Invalidate(EInvalidateWidgetReason::PaintAndVolatility);
    }
    else
    {
        SetCanTick(false);
    }
}

int32 SInoAgentsMessageBubble::OnPaint(
    const FPaintArgs& Args,
    const FGeometry& AllottedGeometry,
    const FSlateRect& MyCullingRect,
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId,
    const FWidgetStyle& InWidgetStyle,
    bool bParentEnabled) const
{
    // Slide up 8 px + fade 0→1 over the curve sequence's duration.
    const float Alpha = EnterSequence.GetLerp();
    const float YOffset = (1.f - Alpha) * 8.f;

    FWidgetStyle StyleCopy = InWidgetStyle;
    StyleCopy.BlendOpacity(Alpha);

    const FGeometry Translated = AllottedGeometry.MakeChild(
        AllottedGeometry.GetLocalSize(),
        FSlateLayoutTransform(FVector2D(0.f, YOffset)));

    return SCompoundWidget::OnPaint(
        Args, Translated, MyCullingRect, OutDrawElements,
        LayerId, StyleCopy, bParentEnabled);
}
