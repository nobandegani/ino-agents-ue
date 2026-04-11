// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

struct FInoAgentsChatStyle;
class SInoAgentsStatusDot;
class STextBlock;
class SButton;

DECLARE_DELEGATE(FOnInoAgentsHeaderAction);

/**
 * Top header strip of the chat panel: title, status dot, theme toggle,
 * close button. Forwards button clicks to the parent panel via simple
 * delegates.
 */
class SInoAgentsChatHeader : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SInoAgentsChatHeader) {}
        SLATE_ARGUMENT(TSharedPtr<FInoAgentsChatStyle>, Style)
        SLATE_EVENT(FOnInoAgentsHeaderAction, OnThemeToggled)
        SLATE_EVENT(FOnInoAgentsHeaderAction, OnDismissed)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Drives the status dot pulse. */
    void SetStreaming(bool bStreaming);

    /** Re-resolve colours/brushes/fonts after a theme toggle. */
    void RefreshStyle(TSharedPtr<FInoAgentsChatStyle> InStyle);

private:
    FReply HandleThemeClicked();
    FReply HandleDismissClicked();

    TSharedPtr<FInoAgentsChatStyle> Style;
    FOnInoAgentsHeaderAction OnThemeToggled;
    FOnInoAgentsHeaderAction OnDismissed;

    TSharedPtr<SInoAgentsStatusDot> StatusDot;
    TSharedPtr<STextBlock> TitleText;
    TSharedPtr<STextBlock> ThemeIconText;
    TSharedPtr<STextBlock> DismissIconText;

    // Stored by value because Slate's button style pointer must outlive
    // the Construct call.
    FButtonStyle IconButtonStyle;
};
