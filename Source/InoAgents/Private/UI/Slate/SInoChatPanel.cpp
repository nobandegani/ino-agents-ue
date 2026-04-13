// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "UI/Slate/SInoChatPanel.h"

#include "InoChatStyle.h"
#include "SInoChatHeader.h"
#include "SInoChatInput.h"
#include "SInoMessageBubble.h"
#include "SInoToolPill.h"

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

void SInoChatPanel::Construct(const FArguments& InArgs)
{
    // Both styles are pinned for the panel's whole lifetime — brushes
    // handed to SBorder/SImage/etc. as raw pointers must outlive every
    // widget that captured them, and theme toggle just re-aims the
    // alias rather than freeing the previous palette.
    DarkStyle  = FInoChatStyle::MakeDark();
    LightStyle = FInoChatStyle::MakeLight();
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
                    SAssignNew(Header, SInoChatHeader)
                    .Style(Style)
                    .OnThemeToggled(FOnInoHeaderAction::CreateSP(
                        this, &SInoChatPanel::HandleThemeToggleFromHeader))
                    .OnDismissed(FOnInoHeaderAction::CreateSP(
                        this, &SInoChatPanel::HandleDismissFromHeader))
                ]
                + SVerticalBox::Slot()
                .FillHeight(1.f)
                .Padding(FMargin(14.f, 0.f, 14.f, 0.f))
                [
                    SAssignNew(MessageScrollBox, SScrollBox)
                    .ScrollBarThickness(FVector2D(6.f, 6.f))
                    .OnUserScrolled(this, &SInoChatPanel::HandleUserScrolled)
                    + SScrollBox::Slot()
                    [
                        SAssignNew(MessageList, SVerticalBox)
                    ]
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SAssignNew(Input, SInoChatInput)
                    .Style(Style)
                    .OnSubmitted(FOnInoChatMessageSubmitted::CreateSP(
                        this, &SInoChatPanel::HandleMessageSubmittedFromInput))
                    .OnCancelRequested(FOnInoChatCancelRequested::CreateSP(
                        this, &SInoChatPanel::HandleCancelFromInput))
                ]
            ]
        ]
    ];

    SetStreaming(false);
}

void SInoChatPanel::PushUserMessage(const FText& Text)
{
    TSharedRef<SInoMessageBubble> Bubble = MakeBubble(EInoBubbleRole::User, Text);
    AppendBubbleToList(Bubble);
    BubbleWidgets.Add(Bubble);
    ActiveAssistantBubble.Reset();
    MaybeAutoScrollToBottom();
}

void SInoChatPanel::AppendAssistantToken(const FString& Chunk)
{
    TSharedPtr<SInoMessageBubble> Existing = ActiveAssistantBubble.Pin();
    if (!Existing.IsValid())
    {
        // Lazy creation: no in-progress assistant bubble, start a new one.
        TSharedRef<SInoMessageBubble> Fresh = MakeBubble(
            EInoBubbleRole::Assistant, FText::GetEmpty());
        AppendBubbleToList(Fresh);
        BubbleWidgets.Add(Fresh);
        Existing = Fresh;
        ActiveAssistantBubble = Fresh;
    }
    Existing->AppendText(Chunk);
    MaybeAutoScrollToBottom();
}

void SInoChatPanel::FinaliseAssistantMessage()
{
    // Per the LiteRtLmConversation contract, OnComplete's FullText is
    // byte-equal to the concatenation of OnToken chunks for the same
    // send. We deliberately do NOT replace the bubble's text — that
    // would risk a flicker and buys nothing. Just clear the active
    // pointer so the next assistant turn creates a fresh bubble.
    ActiveAssistantBubble.Reset();
}

void SInoChatPanel::PushToolCall(
    FName ToolName, const FString& ArgsJson, const FString& ResultJson)
{
    // Tool calls land between assistant text segments. Closing the
    // active assistant bubble here means the next AppendAssistantToken
    // (post-tool-result round) starts a new bubble underneath the pill,
    // which reads correctly: "answer fragment" → "tool call" → "more
    // answer fragment".
    ActiveAssistantBubble.Reset();

    TSharedRef<SInoToolPill> Pill = SNew(SInoToolPill)
        .Style(Style)
        .ToolName(ToolName)
        .ArgumentsJson(ArgsJson)
        .ResultJson(ResultJson);

    AppendBubbleToList(Pill);
    PillWidgets.Add(Pill);
    MaybeAutoScrollToBottom();
}

void SInoChatPanel::SetError(const FText& Message)
{
    ActiveAssistantBubble.Reset();
    TSharedRef<SInoMessageBubble> Bubble = MakeBubble(EInoBubbleRole::Error, Message);
    AppendBubbleToList(Bubble);
    BubbleWidgets.Add(Bubble);
    MaybeAutoScrollToBottom();
}

void SInoChatPanel::SetStreaming(bool bInStreaming)
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

void SInoChatPanel::SetTheme(bool bDark)
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

void SInoChatPanel::FocusInput()
{
    if (!Input.IsValid())
    {
        return;
    }
    // Active timer fires on the next Slate tick, after the panel has
    // been inserted into the viewport — calling SetUserFocus inline
    // from Construct races viewport insertion and silently fails.
    TWeakPtr<SInoChatPanel> WeakSelf = SharedThis(this);
    RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateLambda(
        [WeakSelf](double, float) -> EActiveTimerReturnType
        {
            if (TSharedPtr<SInoChatPanel> Self = WeakSelf.Pin())
            {
                if (Self->Input.IsValid())
                {
                    Self->Input->FocusTextBox();
                }
            }
            return EActiveTimerReturnType::Stop;
        }));
}

void SInoChatPanel::RefreshAllChildStyles()
{
    if (Header.IsValid())
    {
        Header->RefreshStyle(Style);
    }
    if (Input.IsValid())
    {
        Input->RefreshStyle(Style);
    }
    for (const TSharedPtr<SInoMessageBubble>& Bubble : BubbleWidgets)
    {
        if (Bubble.IsValid())
        {
            Bubble->RefreshStyle(Style);
        }
    }
    for (const TSharedPtr<SInoToolPill>& Pill : PillWidgets)
    {
        if (Pill.IsValid())
        {
            Pill->RefreshStyle(Style);
        }
    }
    Invalidate(EInvalidateWidgetReason::Paint | EInvalidateWidgetReason::Layout);
}

TSharedRef<SInoMessageBubble> SInoChatPanel::MakeBubble(
    EInoBubbleRole Role, const FText& InitialText)
{
    return SNew(SInoMessageBubble)
        .Style(Style)
        .Role(Role)
        .InitialText(InitialText);
}

void SInoChatPanel::AppendBubbleToList(TSharedRef<SWidget> Widget)
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

void SInoChatPanel::MaybeAutoScrollToBottom()
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

void SInoChatPanel::HandleMessageSubmittedFromInput(const FString& Text)
{
    PushUserMessage(FText::FromString(Text));
    OnMessageSubmitted.ExecuteIfBound(Text);
}

void SInoChatPanel::HandleCancelFromInput()
{
    OnCancelRequested.ExecuteIfBound();
}

void SInoChatPanel::HandleDismissFromHeader()
{
    OnDismissed.ExecuteIfBound();
}

void SInoChatPanel::HandleThemeToggleFromHeader()
{
    SetTheme(!(Style.IsValid() && Style->bDark));
}

void SInoChatPanel::HandleUserScrolled(float OffsetFraction)
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

FReply SInoChatPanel::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& KeyEvent)
{
    if (KeyEvent.GetKey() == EKeys::Escape)
    {
        OnDismissed.ExecuteIfBound();
        return FReply::Handled();
    }
    return SCompoundWidget::OnKeyDown(MyGeometry, KeyEvent);
}

void SInoChatPanel::Tick(const FGeometry&, const double, const float)
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

int32 SInoChatPanel::OnPaint(
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
