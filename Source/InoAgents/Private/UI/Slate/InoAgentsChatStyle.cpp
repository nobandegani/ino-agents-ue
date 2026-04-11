// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoAgentsChatStyle.h"

#include "Styling/CoreStyle.h"

namespace
{
    // Hex helper that mirrors how the design palette is documented in CLAUDE.md
    // and the plan file: pass 0xRRGGBB literals so the source matches the spec
    // verbatim, no manual /255.f conversions to read past in code review.
    FORCEINLINE FLinearColor FromHex(uint32 RGB, float A = 1.0f)
    {
        return FLinearColor(
            ((RGB >> 16) & 0xFF) / 255.0f,
            ((RGB >>  8) & 0xFF) / 255.0f,
            ( RGB        & 0xFF) / 255.0f,
            A);
    }

    // Common font setup shared by both palettes — typography doesn't change
    // between dark and light modes.
    void PopulateFonts(FInoAgentsChatStyle& S)
    {
        S.HeaderFont = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"),    15);
        S.BodyFont   = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
        S.MetaFont   = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 12);
        S.MonoFont   = FCoreStyle::GetDefaultFontStyle(TEXT("Mono"),    13);
        S.IconFont   = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 16);
    }

    // Build all rounded-box brushes for a populated style. Called after the
    // colour fields have been written so the brushes can reference them.
    void PopulateBrushes(FInoAgentsChatStyle& S)
    {
        // Panel: 16 px radius, 1 px border, surface fill.
        S.PanelBrush = MakeShared<FSlateRoundedBoxBrush>(
            S.PanelBg, 16.f, S.PanelBorder, 1.f);

        // Bubbles: 12 px radius, no border. User and assistant get
        // separate bg colours so the rhythm of the conversation reads
        // even at a glance.
        S.UserBubbleBrush = MakeShared<FSlateRoundedBoxBrush>(
            S.UserBubbleBg, 12.f);
        S.AssistantBubbleBrush = MakeShared<FSlateRoundedBoxBrush>(
            S.AssistantBubbleBg, 12.f);

        // Input row: 10 px radius, 1 px border. Border colour is the
        // accent so the field has visible focus affordance.
        S.InputBrush = MakeShared<FSlateRoundedBoxBrush>(
            S.InputBg, 10.f, S.InputBorder, 1.f);

        // Tool pill: 8 px radius, 1 px border. Border + bg + text are
        // intentionally non-chat colours so a tool call never looks
        // like a regular assistant message.
        S.ToolPillBrush = MakeShared<FSlateRoundedBoxBrush>(
            S.ToolPillBg, 8.f, S.ToolPillBorder, 1.f);

        // Send button: 8 px radius, accent fill, no border. Hover state
        // is a slightly brighter accent.
        S.SendButtonBrush = MakeShared<FSlateRoundedBoxBrush>(
            S.Accent, 8.f);
        S.SendButtonHoverBrush = MakeShared<FSlateRoundedBoxBrush>(
            S.AccentHover, 8.f);

        // Stop button: 8 px radius, error-tinted fill — visually
        // distinct from Send so users do not click the wrong one
        // mid-stream.
        S.StopButtonBrush = MakeShared<FSlateRoundedBoxBrush>(
            S.Error, 8.f);
        S.StopButtonHoverBrush = MakeShared<FSlateRoundedBoxBrush>(
            FLinearColor(S.Error.R + 0.05f, S.Error.G + 0.05f, S.Error.B + 0.05f, 1.f),
            8.f);

        // Icon buttons (close, theme toggle): transparent rest, surface
        // fill on hover. Subtle, doesn't shout for attention.
        S.IconButtonHoverBrush = MakeShared<FSlateRoundedBoxBrush>(
            S.SurfaceBg, 6.f);

        // Status dot: a small filled circle. We approximate "circle" as
        // a rounded box with the radius == half its rendered size; the
        // dot widget passes a fixed 12x12 box so 6px radius is exact.
        S.StatusDotBrush = MakeShared<FSlateRoundedBoxBrush>(
            S.StatusReady, 6.f);
    }
}

TSharedRef<FInoAgentsChatStyle> FInoAgentsChatStyle::MakeDark()
{
    auto S = MakeShared<FInoAgentsChatStyle>();
    S->bDark = true;

    // Dark palette — values straight from the plan file.
    S->PanelBg           = FromHex(0x0F1115);
    S->PanelBorder       = FromHex(0x252A36);
    S->SurfaceBg         = FromHex(0x171A21);
    S->UserBubbleBg      = FromHex(0x1E2230);
    S->AssistantBubbleBg = FromHex(0x23283A);
    S->InputBg           = FromHex(0x171A21);
    S->InputBorder       = FromHex(0x2B3144);

    S->TextPrimary       = FromHex(0xECEEF3);
    S->TextMuted         = FromHex(0x8B92A5);
    S->TextOnAccent      = FromHex(0x0F1115);
    S->Accent            = FromHex(0x7C93FF);
    S->AccentHover       = FromHex(0x96A8FF);
    S->Error             = FromHex(0xE5707A);

    S->ToolPillBg        = FromHex(0x2A2F1F);
    S->ToolPillText      = FromHex(0xC9D66B);
    S->ToolPillBorder    = FromHex(0x3A4127);

    S->StatusReady       = FromHex(0x7C93FF);
    S->StatusBusy        = FromHex(0xFCB544);

    PopulateFonts(*S);
    PopulateBrushes(*S);
    return S;
}

TSharedRef<FInoAgentsChatStyle> FInoAgentsChatStyle::MakeLight()
{
    auto S = MakeShared<FInoAgentsChatStyle>();
    S->bDark = false;

    // Light palette — same hue family as dark, inverted luminance, with
    // tweaks where direct inversion would have looked muddy.
    S->PanelBg           = FromHex(0xF6F7FB);
    S->PanelBorder       = FromHex(0xDCE0EA);
    S->SurfaceBg         = FromHex(0xEFF1F7);
    S->UserBubbleBg      = FromHex(0xE3E8FA);  // tinted toward accent
    S->AssistantBubbleBg = FromHex(0xFFFFFF);
    S->InputBg           = FromHex(0xFFFFFF);
    S->InputBorder       = FromHex(0xCBD2E2);

    S->TextPrimary       = FromHex(0x141826);
    S->TextMuted         = FromHex(0x6C7488);
    S->TextOnAccent      = FromHex(0xFFFFFF);
    S->Accent            = FromHex(0x4458D8);
    S->AccentHover       = FromHex(0x5468EA);
    S->Error             = FromHex(0xC73645);

    S->ToolPillBg        = FromHex(0xF1F4DA);
    S->ToolPillText      = FromHex(0x546018);
    S->ToolPillBorder    = FromHex(0xCBD2A0);

    S->StatusReady       = FromHex(0x4458D8);
    S->StatusBusy        = FromHex(0xD27800);

    PopulateFonts(*S);
    PopulateBrushes(*S);
    return S;
}
