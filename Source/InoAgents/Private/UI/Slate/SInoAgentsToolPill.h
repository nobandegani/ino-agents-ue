// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Animation/CurveSequence.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

struct FInoAgentsChatStyle;
class SBorder;
class STextBlock;

/**
 * Compact display of one tool call inside the message stream.
 *
 * Header line:
 *     toolname → "<truncated result>"
 *
 * The result is truncated with an ellipsis (OverflowPolicy::Ellipsis on
 * the inner STextBlock). Hovering reveals an SToolTip with the full
 * arguments JSON and result JSON in a Mono-font scrollable region.
 *
 * The pill plays a small enter animation (slide up 6 px + fade) on
 * Construct so it doesn't pop into the message list cold.
 */
class SInoAgentsToolPill : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SInoAgentsToolPill) {}
        SLATE_ARGUMENT(TSharedPtr<FInoAgentsChatStyle>, Style)
        SLATE_ARGUMENT(FName, ToolName)
        SLATE_ARGUMENT(FString, ArgumentsJson)
        SLATE_ARGUMENT(FString, ResultJson)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Re-resolve colours/brushes/fonts after a theme toggle. */
    void RefreshStyle(TSharedPtr<FInoAgentsChatStyle> InStyle);

private:
    void Tick(const FGeometry&, const double, const float) override;
    int32 OnPaint(
        const FPaintArgs&,
        const FGeometry&,
        const FSlateRect&,
        FSlateWindowElementList&,
        int32,
        const FWidgetStyle&,
        bool) const override;

    FString BuildHeadlineText() const;
    FString BuildTooltipText() const;

    TSharedPtr<FInoAgentsChatStyle> Style;
    FName ToolName;
    FString ArgumentsJson;
    FString ResultJson;

    TSharedPtr<SBorder>    PillBorder;
    TSharedPtr<STextBlock> HeadlineText;
    TSharedPtr<STextBlock> GearGlyph;

    FCurveSequence EnterSequence;
};
