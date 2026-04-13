// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

struct FInoChatStyle;
class SInoStatusDot;
class STextBlock;
class SButton;

DECLARE_DELEGATE(FOnInoHeaderAction);

/**
 * Top header strip of the chat panel: title, status dot, theme toggle,
 * close button. Forwards button clicks to the parent panel via simple
 * delegates.
 */
class SInoChatHeader : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SInoChatHeader) {}
        SLATE_ARGUMENT(TSharedPtr<FInoChatStyle>, Style)
        SLATE_EVENT(FOnInoHeaderAction, OnThemeToggled)
        SLATE_EVENT(FOnInoHeaderAction, OnDismissed)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Drives the status dot pulse. */
    void SetStreaming(bool bStreaming);

    /** Re-resolve colours/brushes/fonts after a theme toggle. */
    void RefreshStyle(TSharedPtr<FInoChatStyle> InStyle);

private:
    FReply HandleThemeClicked();
    FReply HandleDismissClicked();

    TSharedPtr<FInoChatStyle> Style;
    FOnInoHeaderAction OnThemeToggled;
    FOnInoHeaderAction OnDismissed;

    TSharedPtr<SInoStatusDot> StatusDot;
    TSharedPtr<STextBlock> TitleText;
    TSharedPtr<STextBlock> ThemeIconText;
    TSharedPtr<STextBlock> DismissIconText;

    // Stored by value because Slate's button style pointer must outlive
    // the Construct call.
    FButtonStyle IconButtonStyle;
};
