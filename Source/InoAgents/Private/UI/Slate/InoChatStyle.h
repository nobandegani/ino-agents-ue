// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateColor.h"

/**
 * Pure-code style descriptor for the InoAgents chat panel.
 *
 * Holds the colour palette, rounded-box brushes, and font info that every
 * widget in the chat tree consumes. Two factories: MakeDark() / MakeLight().
 *
 * Brushes are TSharedPtr<FSlateRoundedBoxBrush> so theme toggle can swap the
 * outer FInoChatStyle ref atomically — bubbles that captured the prior
 * shared ref still see a valid brush until they re-resolve from the new
 * style at the next paint.
 */
struct FInoChatStyle
{
    bool bDark = true;

    // ----- colours -----
    FLinearColor PanelBg          = FLinearColor::Black;
    FLinearColor PanelBorder      = FLinearColor::Black;
    FLinearColor SurfaceBg        = FLinearColor::Black;
    FLinearColor UserBubbleBg     = FLinearColor::Black;
    FLinearColor AssistantBubbleBg= FLinearColor::Black;
    FLinearColor InputBg          = FLinearColor::Black;
    FLinearColor InputBorder      = FLinearColor::Black;

    FLinearColor TextPrimary      = FLinearColor::White;
    FLinearColor TextMuted        = FLinearColor::Gray;
    FLinearColor TextOnAccent     = FLinearColor::White;
    FLinearColor Accent           = FLinearColor(0.49f, 0.58f, 1.f);
    FLinearColor AccentHover      = FLinearColor(0.59f, 0.68f, 1.f);
    FLinearColor Error            = FLinearColor(1.f, 0.45f, 0.45f);

    FLinearColor ToolPillBg       = FLinearColor::Black;
    FLinearColor ToolPillText     = FLinearColor::White;
    FLinearColor ToolPillBorder   = FLinearColor::Black;

    FLinearColor StatusReady      = FLinearColor(0.49f, 0.58f, 1.f);
    FLinearColor StatusBusy       = FLinearColor(0.99f, 0.71f, 0.27f);

    // ----- brushes (rounded-box, no image asset required) -----
    TSharedPtr<FSlateRoundedBoxBrush> PanelBrush;
    TSharedPtr<FSlateRoundedBoxBrush> UserBubbleBrush;
    TSharedPtr<FSlateRoundedBoxBrush> AssistantBubbleBrush;
    TSharedPtr<FSlateRoundedBoxBrush> InputBrush;
    TSharedPtr<FSlateRoundedBoxBrush> ToolPillBrush;
    TSharedPtr<FSlateRoundedBoxBrush> SendButtonBrush;
    TSharedPtr<FSlateRoundedBoxBrush> SendButtonHoverBrush;
    TSharedPtr<FSlateRoundedBoxBrush> StopButtonBrush;
    TSharedPtr<FSlateRoundedBoxBrush> StopButtonHoverBrush;
    TSharedPtr<FSlateRoundedBoxBrush> IconButtonHoverBrush;
    TSharedPtr<FSlateRoundedBoxBrush> StatusDotBrush;

    // ----- fonts (engine-bundled Roboto via FCoreStyle, no asset cooking) -----
    FSlateFontInfo HeaderFont;   // 15 Bold
    FSlateFontInfo BodyFont;     // 14 Regular
    FSlateFontInfo MetaFont;     // 12 Regular
    FSlateFontInfo MonoFont;     // 13 Mono — tool-pill tooltip
    FSlateFontInfo IconFont;     // 16 Regular — close/theme glyphs

    // Two factories. Returned as TSharedRef so the panel can hot-swap on
    // theme toggle without disturbing widgets that captured the prior ref.
    static TSharedRef<FInoChatStyle> MakeDark();
    static TSharedRef<FInoChatStyle> MakeLight();
};
