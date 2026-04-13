// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "SInoToolPill.h"

#include "InoChatStyle.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SToolTip.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
    constexpr int32 kHeadlineResultMaxChars = 48;
}

void SInoToolPill::Construct(const FArguments& InArgs)
{
    Style         = InArgs._Style;
    ToolName      = InArgs._ToolName;
    ArgumentsJson = InArgs._ArgumentsJson;
    ResultJson    = InArgs._ResultJson;

    EnterSequence = FCurveSequence();
    EnterSequence.AddCurve(0.f, 0.18f, ECurveEaseFunction::CubicOut);
    EnterSequence.Play(SharedThis(this));
    SetCanTick(true);

    const FLinearColor PillText   = Style.IsValid() ? Style->ToolPillText  : FLinearColor::White;
    const FSlateFontInfo BodyFont = Style.IsValid() ? Style->BodyFont      : FSlateFontInfo();
    const FSlateFontInfo MonoFont = Style.IsValid() ? Style->MonoFont      : FSlateFontInfo();
    const FSlateFontInfo MetaFont = Style.IsValid() ? Style->MetaFont      : FSlateFontInfo();
    const FSlateBrush*   PillBg   = Style.IsValid() && Style->ToolPillBrush.IsValid()
        ? Style->ToolPillBrush.Get()
        : FCoreStyle::Get().GetBrush("WhiteBrush");

    // Build the tooltip content first so we can hand it to SToolTip.
    const TSharedRef<SWidget> TooltipContent =
        SNew(SBox)
        .MaxDesiredWidth(420.f)
        .MaxDesiredHeight(260.f)
        .Padding(FMargin(10.f))
        [
            SNew(SScrollBox)
            + SScrollBox::Slot()
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(FMargin(0.f, 0.f, 0.f, 4.f))
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(FString::Printf(
                        TEXT("%s — arguments"), *ToolName.ToString())))
                    .Font(MetaFont)
                    .ColorAndOpacity(FSlateColor(PillText))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(FMargin(0.f, 0.f, 0.f, 10.f))
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(ArgumentsJson.IsEmpty()
                        ? FString(TEXT("{}"))
                        : ArgumentsJson))
                    .Font(MonoFont)
                    .ColorAndOpacity(FSlateColor(FLinearColor::White))
                    .AutoWrapText(true)
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(FMargin(0.f, 0.f, 0.f, 4.f))
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("result")))
                    .Font(MetaFont)
                    .ColorAndOpacity(FSlateColor(PillText))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(ResultJson.IsEmpty()
                        ? FString(TEXT("(none)"))
                        : ResultJson))
                    .Font(MonoFont)
                    .ColorAndOpacity(FSlateColor(FLinearColor::White))
                    .AutoWrapText(true)
                ]
            ]
        ];

    // Pill body: rounded border with one truncated headline line.
    ChildSlot
    .Padding(FMargin(0.f))
    [
        SAssignNew(PillBorder, SBorder)
        .BorderImage(PillBg)
        .Padding(FMargin(10.f, 6.f))
        .ToolTip(SNew(SToolTip)[ TooltipContent ])
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(FMargin(0.f, 0.f, 6.f, 0.f))
            [
                // Bullet glyph keeps the line readable when the model name is long.
                SAssignNew(GearGlyph, STextBlock)
                .Text(FText::FromString(TEXT("⚙")))
                .Font(MetaFont)
                .ColorAndOpacity(FSlateColor(PillText))
            ]
            + SHorizontalBox::Slot()
            .FillWidth(1.f)
            .VAlign(VAlign_Center)
            [
                SAssignNew(HeadlineText, STextBlock)
                .Text(FText::FromString(BuildHeadlineText()))
                .Font(BodyFont)
                .ColorAndOpacity(FSlateColor(PillText))
                .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
            ]
        ]
    ];
}

void SInoToolPill::RefreshStyle(TSharedPtr<FInoChatStyle> InStyle)
{
    Style = InStyle;
    if (!Style.IsValid())
    {
        return;
    }

    if (PillBorder.IsValid() && Style->ToolPillBrush.IsValid())
    {
        PillBorder->SetBorderImage(Style->ToolPillBrush.Get());
    }
    if (HeadlineText.IsValid())
    {
        HeadlineText->SetColorAndOpacity(FSlateColor(Style->ToolPillText));
        HeadlineText->SetFont(Style->BodyFont);
    }
    if (GearGlyph.IsValid())
    {
        GearGlyph->SetColorAndOpacity(FSlateColor(Style->ToolPillText));
        GearGlyph->SetFont(Style->MetaFont);
    }
    Invalidate(EInvalidateWidgetReason::Paint | EInvalidateWidgetReason::Layout);
}

FString SInoToolPill::BuildHeadlineText() const
{
    FString Result = ResultJson;
    if (Result.Len() > kHeadlineResultMaxChars)
    {
        Result = Result.Left(kHeadlineResultMaxChars - 1) + TEXT("…");
    }
    return FString::Printf(TEXT("%s → %s"), *ToolName.ToString(), *Result);
}

FString SInoToolPill::BuildTooltipText() const
{
    return FString::Printf(TEXT("%s\nargs: %s\nresult: %s"),
        *ToolName.ToString(), *ArgumentsJson, *ResultJson);
}

void SInoToolPill::Tick(const FGeometry& AllottedGeometry, const double, const float)
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

int32 SInoToolPill::OnPaint(
    const FPaintArgs& Args,
    const FGeometry& AllottedGeometry,
    const FSlateRect& MyCullingRect,
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId,
    const FWidgetStyle& InWidgetStyle,
    bool bParentEnabled) const
{
    // Apply enter animation: slide 6 px upward from rest, fade 0→1.
    const float Alpha = EnterSequence.GetLerp();
    const float YOffset = (1.f - Alpha) * 6.f;

    FWidgetStyle StyleCopy = InWidgetStyle;
    StyleCopy.BlendOpacity(Alpha);

    const FGeometry Translated = AllottedGeometry.MakeChild(
        AllottedGeometry.GetLocalSize(),
        FSlateLayoutTransform(FVector2D(0.f, YOffset)));

    return SCompoundWidget::OnPaint(
        Args, Translated, MyCullingRect, OutDrawElements,
        LayerId, StyleCopy, bParentEnabled);
}
