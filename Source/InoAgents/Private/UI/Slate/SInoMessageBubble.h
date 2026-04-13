// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Animation/CurveSequence.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

struct FInoChatStyle;
class STextBlock;

/**
 * One bubble in the chat scroll. Two visual variants:
 *   - User: anchored to the right, accent-tinted bubble bg.
 *   - Assistant: anchored to the left, surface-tinted bubble bg, supports
 *     streaming token mutation via AppendText().
 *
 * Bubbles capture a snapshot of the panel's FInoChatStyle at
 * Construct time but receive RefreshStyle() callbacks from the panel
 * after a theme toggle so colours/brushes update without rebuilding the
 * widget tree (which would replay enter animations on every existing
 * bubble — looks awful).
 *
 * Each bubble plays a one-shot enter animation (slide up 8 px + fade) on
 * Construct via FCurveSequence + Tick-and-Invalidate. The animation runs
 * exactly once per bubble — never replayed on theme toggle.
 */
enum class EInoBubbleRole : uint8
{
    User,
    Assistant,
    Error,
};

class SInoMessageBubble : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SInoMessageBubble) {}
        SLATE_ARGUMENT(TSharedPtr<FInoChatStyle>, Style)
        SLATE_ARGUMENT(EInoBubbleRole, Role)
        SLATE_ARGUMENT(FText, InitialText)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Replace the bubble's text outright. Used by PushUserMessage / SetError. */
    void SetText(const FText& InText);

    /** Append a streaming token. Used during assistant message streaming. */
    void AppendText(const FString& Chunk);

    /** Read current text — used by the panel for the OnComplete equivalence assert. */
    FText GetText() const;

    /** Re-resolve colours/brushes/fonts after a theme toggle. */
    void RefreshStyle(TSharedPtr<FInoChatStyle> InStyle);

    EInoBubbleRole GetRole() const { return Role; }

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

    void RebuildContent();

    TSharedPtr<FInoChatStyle> Style;
    EInoBubbleRole Role = EInoBubbleRole::Assistant;
    FText CurrentText;

    TSharedPtr<STextBlock> TextBlock;
    FCurveSequence EnterSequence;
};
