// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Animation/CurveSequence.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

struct FInoAgentsChatStyle;

/**
 * Small filled dot in the chat header that pulses while a stream is in
 * flight and is solid otherwise. Coloured according to the panel style:
 * StatusReady when idle, StatusBusy while streaming.
 *
 * The pulse is implemented via SetVolatile(true) when streaming flips on,
 * and a continuous FCurveSequence that drives a 1.0↔1.15 scale on the
 * dot's render transform. Volatile widgets paint every frame without
 * explicit Invalidate calls — the idiomatic Slate choice for a
 * perpetually-animating sub-region.
 */
class SInoAgentsStatusDot : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SInoAgentsStatusDot) {}
        SLATE_ARGUMENT(TSharedPtr<FInoAgentsChatStyle>, Style)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Toggle the pulse + colour swap. Idempotent. */
    void SetStreaming(bool bInStreaming);

    /** Hot-swap the style after a theme toggle. Triggers a repaint. */
    void SetStyle(TSharedPtr<FInoAgentsChatStyle> InStyle);

private:
    int32 OnPaint(
        const FPaintArgs& Args,
        const FGeometry& AllottedGeometry,
        const FSlateRect& MyCullingRect,
        FSlateWindowElementList& OutDrawElements,
        int32 LayerId,
        const FWidgetStyle& InWidgetStyle,
        bool bParentEnabled) const override;

    void Tick(const FGeometry&, const double, const float) override;

    FVector2D ComputeDesiredSize(float) const override;

    TSharedPtr<FInoAgentsChatStyle> Style;
    FCurveSequence PulseSequence;
    bool bStreaming = false;
};
