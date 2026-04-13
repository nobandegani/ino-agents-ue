// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "SInoStatusDot.h"

#include "InoChatStyle.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Rendering/DrawElements.h"

namespace
{
    constexpr float kDotSize = 12.f;
}

void SInoStatusDot::Construct(const FArguments& InArgs)
{
    Style = InArgs._Style;

    // Pulse runs forever once started; we toggle play/stop via
    // SetStreaming. Duration is intentionally slow (1.2s) so the dot
    // breathes rather than flashes.
    PulseSequence = FCurveSequence();
    PulseSequence.AddCurve(0.f, 1.2f, ECurveEaseFunction::CubicInOut);
}

void SInoStatusDot::SetStreaming(bool bInStreaming)
{
    if (bStreaming == bInStreaming)
    {
        return;
    }
    bStreaming = bInStreaming;

    if (bStreaming)
    {
        // Drive the pulse from Tick — we re-Invalidate every frame
        // while the curve is playing, then stop ticking when streaming
        // ends. Same pattern the bubbles and panel use for their
        // one-shot enter animations, just looping.
        SetCanTick(true);
        PulseSequence.Play(SharedThis(this), /*bPlayLooped=*/true);
    }
    else
    {
        PulseSequence.JumpToStart();
        SetCanTick(false);
        Invalidate(EInvalidateWidgetReason::Paint);
    }
}

void SInoStatusDot::Tick(const FGeometry&, const double, const float)
{
    if (bStreaming && PulseSequence.IsPlaying())
    {
        Invalidate(EInvalidateWidgetReason::PaintAndVolatility);
    }
}

void SInoStatusDot::SetStyle(TSharedPtr<FInoChatStyle> InStyle)
{
    Style = InStyle;
    Invalidate(EInvalidateWidgetReason::Paint);
}

FVector2D SInoStatusDot::ComputeDesiredSize(float) const
{
    // Reserve a touch of headroom around the dot so the scaled-up pulse
    // peak doesn't get clipped by the parent box.
    return FVector2D(kDotSize + 4.f, kDotSize + 4.f);
}

int32 SInoStatusDot::OnPaint(
    const FPaintArgs& Args,
    const FGeometry& AllottedGeometry,
    const FSlateRect& MyCullingRect,
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId,
    const FWidgetStyle& InWidgetStyle,
    bool bParentEnabled) const
{
    if (!Style.IsValid() || !Style->StatusDotBrush.IsValid())
    {
        return LayerId;
    }

    // Recolour the brush in place each paint — we have one shared brush
    // per style, so writing to TintColor here is fine: it's our brush.
    const FLinearColor DotColour = bStreaming ? Style->StatusBusy : Style->StatusReady;
    Style->StatusDotBrush->TintColor = FSlateColor(DotColour);

    // Pulse: scale 1.00 ↔ 1.15 driven by the curve sequence's lerp
    // value. When not streaming the lerp is 0 and scale is exactly 1.
    const float Lerp = bStreaming ? PulseSequence.GetLerp() : 0.f;
    const float Scale = 1.f + 0.15f * Lerp;

    const FVector2D AllottedSize = AllottedGeometry.GetLocalSize();
    const FVector2D DrawSize(kDotSize * Scale, kDotSize * Scale);
    const FVector2D Centre = AllottedSize * 0.5f;
    const FVector2D Offset = Centre - DrawSize * 0.5f;

    const FPaintGeometry PaintGeo = AllottedGeometry.ToPaintGeometry(
        DrawSize, FSlateLayoutTransform(Offset));

    FSlateDrawElement::MakeBox(
        OutDrawElements,
        LayerId,
        PaintGeo,
        Style->StatusDotBrush.Get(),
        ESlateDrawEffect::None,
        DotColour);

    return LayerId + 1;
}
