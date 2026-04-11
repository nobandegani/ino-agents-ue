// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "UI/Slate/SInoAgentsChatPanel.h"

#include "InoAgentsChatStyle.h"
#include "SInoAgentsChatHeader.h"
#include "SInoAgentsChatInput.h"
#include "SInoAgentsMessageBubble.h"
#include "SInoAgentsToolPill.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"

namespace
{
    constexpr float kPanelWidth  = 520.f;
    constexpr float kPanelHeight = 720.f;
}

void SInoAgentsChatPanel::Construct(const FArguments& InArgs)
{
    // Both styles are pinned for the panel's whole lifetime — brushes
    // handed to SBorder/SImage/etc. as raw pointers must outlive every
    // widget that captured them, and theme toggle just re-aims the
    // alias rather than freeing the previous palette.
    DarkStyle  = FInoAgentsChatStyle::MakeDark();
    LightStyle = FInoAgentsChatStyle::MakeLight();
    Style      = DarkStyle;

    OnMessageSubmitted = InArgs._OnMessageSubmitted;
    OnDismissed        = InArgs._OnDismissed;
    OnCancelRequested  = InArgs._OnCancelRequested;

    EnterSequence = FCurveSequence();
    EnterSequence.AddCurve(0.f, 0.22f, ECurveEaseFunction::CubicOut);
    EnterSequence.Play(SharedThis(this));
    SetCanTick(true);

    // Build the panel: rounded SBorder around a vertical stack of
    // header / scrolling message list / input row.
    ChildSlot
    [
        SNew(SBox)
        .WidthOverride(kPanelWidth)
        .HeightOverride(kPanelHeight)
        [
            SAssignNew(PanelBorder, SBorder)
            .BorderImage(Style->PanelBrush.Get())
            .Padding(FMargin(0.f))
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SAssignNew(Header, SInoAgentsChatHeader)
                    .Style(Style)
                    .OnThemeToggled(FOnInoAgentsHeaderAction::CreateSP(
                        this, &SInoAgentsChatPanel::HandleThemeToggleFromHeader))
                    .OnDismissed(FOnInoAgentsHeaderAction::CreateSP(
                        this, &SInoAgentsChatPanel::HandleDismissFromHeader))
                ]
                + SVerticalBox::Slot()
                .FillHeight(1.f)
                .Padding(FMargin(14.f, 0.f, 14.f, 0.f))
                [
                    SAssignNew(MessageScrollBox, SScrollBox)
                    .ScrollBarThickness(FVector2D(6.f, 6.f))
                    .OnUserScrolled(this, &SInoAgentsChatPanel::HandleUserScrolled)
                    + SScrollBox::Slot()
                    [
                        SAssignNew(MessageList, SVerticalBox)
                    ]
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SAssignNew(Input, SInoAgentsChatInput)
                    .Style(Style)
                    .OnSubmitted(FOnInoAgentsChatMessageSubmitted::CreateSP(
                        this, &SInoAgentsChatPanel::HandleMessageSubmittedFromInput))
                    .OnCancelRequested(FOnInoAgentsChatCancelRequested::CreateSP(
                        this, &SInoAgentsChatPanel::HandleCancelFromInput))
                ]
            ]
        ]
    ];

    SetStreaming(false);
}

void SInoAgentsChatPanel::PushUserMessage(const FText& Text)
{
    TSharedRef<SInoAgentsMessageBubble> Bubble = MakeBubble(EInoAgentsBubbleRole::User, Text);
    AppendBubbleToList(Bubble);
    BubbleWidgets.Add(Bubble);
    ActiveAssistantBubble.Reset();
    MaybeAutoScrollToBottom();
}

void SInoAgentsChatPanel::AppendAssistantToken(const FString& Chunk)
{
    TSharedPtr<SInoAgentsMessageBubble> Existing = ActiveAssistantBubble.Pin();
    if (!Existing.IsValid())
    {
        // Lazy creation: no in-progress assistant bubble, start a new one.
        TSharedRef<SInoAgentsMessageBubble> Fresh = MakeBubble(
            EInoAgentsBubbleRole::Assistant, FText::GetEmpty());
        AppendBubbleToList(Fresh);
        BubbleWidgets.Add(Fresh);
        Existing = Fresh;
        ActiveAssistantBubble = Fresh;
    }
    Existing->AppendText(Chunk);
    MaybeAutoScrollToBottom();
}

void SInoAgentsChatPanel::FinaliseAssistantMessage()
{
    // Per the LiteRtLmConversation contract, OnComplete's FullText is
    // byte-equal to the concatenation of OnToken chunks for the same
    // send. We deliberately do NOT replace the bubble's text — that
    // would risk a flicker and buys nothing. Just clear the active
    // pointer so the next assistant turn creates a fresh bubble.
    ActiveAssistantBubble.Reset();
}

void SInoAgentsChatPanel::PushToolCall(
    FName ToolName, const FString& ArgsJson, const FString& ResultJson)
{
    // Tool calls land between assistant text segments. Closing the
    // active assistant bubble here means the next AppendAssistantToken
    // (post-tool-result round) starts a new bubble underneath the pill,
    // which reads correctly: "answer fragment" → "tool call" → "more
    // answer fragment".
    ActiveAssistantBubble.Reset();

    TSharedRef<SInoAgentsToolPill> Pill = SNew(SInoAgentsToolPill)
        .Style(Style)
        .ToolName(ToolName)
        .ArgumentsJson(ArgsJson)
        .ResultJson(ResultJson);

    AppendBubbleToList(Pill);
    PillWidgets.Add(Pill);
    MaybeAutoScrollToBottom();
}

void SInoAgentsChatPanel::SetError(const FText& Message)
{
    ActiveAssistantBubble.Reset();
    TSharedRef<SInoAgentsMessageBubble> Bubble = MakeBubble(EInoAgentsBubbleRole::Error, Message);
    AppendBubbleToList(Bubble);
    BubbleWidgets.Add(Bubble);
    MaybeAutoScrollToBottom();
}

void SInoAgentsChatPanel::SetStreaming(bool bInStreaming)
{
    bStreaming = bInStreaming;
    if (Header.IsValid())
    {
        Header->SetStreaming(bStreaming);
    }
    if (Input.IsValid())
    {
        Input->SetStreaming(bStreaming);
    }
}

void SInoAgentsChatPanel::SetTheme(bool bDark)
{
    if (Style.IsValid() && Style->bDark == bDark)
    {
        return;
    }
    // Just re-aim the alias — both palettes are already pinned, so any
    // raw brush pointers held by SBorder/SImage from the prior theme
    // remain valid until we explicitly repoint them below.
    Style = bDark ? DarkStyle : LightStyle;

    // Repoint our own outer border at the new panel brush. Children
    // (header, input, bubbles, pills) get RefreshStyle and rebuild
    // their own borders against the new style.
    if (PanelBorder.IsValid() && Style.IsValid() && Style->PanelBrush.IsValid())
    {
        PanelBorder->SetBorderImage(Style->PanelBrush.Get());
    }

    RefreshAllChildStyles();
}

void SInoAgentsChatPanel::FocusInput()
{
    if (!Input.IsValid())
    {
        return;
    }
    // Active timer fires on the next Slate tick, after the panel has
    // been inserted into the viewport — calling SetUserFocus inline
    // from Construct races viewport insertion and silently fails.
    TWeakPtr<SInoAgentsChatPanel> WeakSelf = SharedThis(this);
    RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateLambda(
        [WeakSelf](double, float) -> EActiveTimerReturnType
        {
            if (TSharedPtr<SInoAgentsChatPanel> Self = WeakSelf.Pin())
            {
                if (Self->Input.IsValid())
                {
                    Self->Input->FocusTextBox();
                }
            }
            return EActiveTimerReturnType::Stop;
        }));
}

void SInoAgentsChatPanel::RefreshAllChildStyles()
{
    if (Header.IsValid())
    {
        Header->RefreshStyle(Style);
    }
    if (Input.IsValid())
    {
        Input->RefreshStyle(Style);
    }
    for (const TSharedPtr<SInoAgentsMessageBubble>& Bubble : BubbleWidgets)
    {
        if (Bubble.IsValid())
        {
            Bubble->RefreshStyle(Style);
        }
    }
    for (const TSharedPtr<SInoAgentsToolPill>& Pill : PillWidgets)
    {
        if (Pill.IsValid())
        {
            Pill->RefreshStyle(Style);
        }
    }
    Invalidate(EInvalidateWidgetReason::Paint | EInvalidateWidgetReason::Layout);
}

TSharedRef<SInoAgentsMessageBubble> SInoAgentsChatPanel::MakeBubble(
    EInoAgentsBubbleRole Role, const FText& InitialText)
{
    return SNew(SInoAgentsMessageBubble)
        .Style(Style)
        .Role(Role)
        .InitialText(InitialText);
}

void SInoAgentsChatPanel::AppendBubbleToList(TSharedRef<SWidget> Widget)
{
    if (!MessageList.IsValid())
    {
        return;
    }
    MessageList->AddSlot()
        .AutoHeight()
        [
            Widget
        ];
}

void SInoAgentsChatPanel::MaybeAutoScrollToBottom()
{
    if (!MessageScrollBox.IsValid())
    {
        return;
    }
    if (bUserScrolledUp)
    {
        return;
    }
    MessageScrollBox->ScrollToEnd();
}

void SInoAgentsChatPanel::HandleMessageSubmittedFromInput(const FString& Text)
{
    PushUserMessage(FText::FromString(Text));
    OnMessageSubmitted.ExecuteIfBound(Text);
}

void SInoAgentsChatPanel::HandleCancelFromInput()
{
    OnCancelRequested.ExecuteIfBound();
}

void SInoAgentsChatPanel::HandleDismissFromHeader()
{
    OnDismissed.ExecuteIfBound();
}

void SInoAgentsChatPanel::HandleThemeToggleFromHeader()
{
    SetTheme(!(Style.IsValid() && Style->bDark));
}

void SInoAgentsChatPanel::HandleUserScrolled(float OffsetFraction)
{
    // Detect "user scrolled away from bottom". Tolerance of 4 px in
    // fraction terms is impossible to compute without knowing total
    // size, so we approximate: any value < 0.98 is "scrolled up".
    if (!MessageScrollBox.IsValid())
    {
        return;
    }
    const float Distance = MessageScrollBox->GetScrollOffsetOfEnd();
    const float Current  = MessageScrollBox->GetScrollOffset();
    bUserScrolledUp = (Distance - Current) > 4.f;
}

FReply SInoAgentsChatPanel::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& KeyEvent)
{
    if (KeyEvent.GetKey() == EKeys::Escape)
    {
        OnDismissed.ExecuteIfBound();
        return FReply::Handled();
    }
    return SCompoundWidget::OnKeyDown(MyGeometry, KeyEvent);
}

void SInoAgentsChatPanel::Tick(const FGeometry&, const double, const float)
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

int32 SInoAgentsChatPanel::OnPaint(
    const FPaintArgs& Args,
    const FGeometry& AllottedGeometry,
    const FSlateRect& MyCullingRect,
    FSlateWindowElementList& OutDrawElements,
    int32 LayerId,
    const FWidgetStyle& InWidgetStyle,
    bool bParentEnabled) const
{
    // Panel enter animation: slide up 16 px + fade.
    const float Alpha = EnterSequence.GetLerp();
    const float YOffset = (1.f - Alpha) * 16.f;

    FWidgetStyle StyleCopy = InWidgetStyle;
    StyleCopy.BlendOpacity(Alpha);

    const FGeometry Translated = AllottedGeometry.MakeChild(
        AllottedGeometry.GetLocalSize(),
        FSlateLayoutTransform(FVector2D(0.f, YOffset)));

    return SCompoundWidget::OnPaint(
        Args, Translated, MyCullingRect, OutDrawElements,
        LayerId, StyleCopy, bParentEnabled);
}
