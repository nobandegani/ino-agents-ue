// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "SInoMessageBubble.h"

#include "InoChatStyle.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SInoMessageBubble::Construct(const FArguments& InArgs)
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

void SInoMessageBubble::RebuildContent()
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
    case EInoBubbleRole::User:
        BubbleBg = Style->UserBubbleBrush.IsValid()
            ? Style->UserBubbleBrush.Get()
            : FCoreStyle::Get().GetBrush("WhiteBrush");
        HAlign = HAlign_Right;
        TextColour = Style->TextPrimary;
        break;

    case EInoBubbleRole::Assistant:
        BubbleBg = Style->AssistantBubbleBrush.IsValid()
            ? Style->AssistantBubbleBrush.Get()
            : FCoreStyle::Get().GetBrush("WhiteBrush");
        HAlign = HAlign_Left;
        TextColour = Style->TextPrimary;
        break;

    case EInoBubbleRole::Error:
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
        // WrapTextAt (hard pixel cap) rather than AutoWrapText (needs the
        // parent to push a width) — our ChildSlot uses HAlign_Left/Right
        // which shrinks to the child's desired size, and an AutoWrap text
        // block reports its desired width as the LONGEST WORD. Result
        // was 5-char-wide bubbles. 392 = 416 outer cap − 2*12 padding.
        SNew(SBorder)
        .BorderImage(BubbleBg)
        .Padding(FMargin(12.f, 9.f))
        [
            SAssignNew(TextBlock, STextBlock)
            .Text(CurrentText)
            .Font(Style->BodyFont)
            .ColorAndOpacity(FSlateColor(TextColour))
            .WrapTextAt(392.f)
        ]
    ];
}

void SInoMessageBubble::SetText(const FText& InText)
{
    CurrentText = InText;
    if (TextBlock.IsValid())
    {
        TextBlock->SetText(CurrentText);
    }
}

void SInoMessageBubble::AppendText(const FString& Chunk)
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

FText SInoMessageBubble::GetText() const
{
    return CurrentText;
}

void SInoMessageBubble::RefreshStyle(TSharedPtr<FInoChatStyle> InStyle)
{
    Style = InStyle;
    RebuildContent();
}

void SInoMessageBubble::Tick(const FGeometry&, const double, const float)
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

int32 SInoMessageBubble::OnPaint(
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
