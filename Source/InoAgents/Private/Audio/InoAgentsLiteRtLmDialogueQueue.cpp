// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoAgentsLiteRtLmDialogueQueue.h"

#include "Audio/InoAgentsStreamingAudioComponent.h"
#include "ElevenLabs/ElevenLabsTextToDialogueStream.h"
#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmConversation.h"

#include "Containers/Ticker.h"

// ======================================================================
// Slot observer — per-TTS-action trampoline
// ======================================================================

void UInoAgentsLiteRtLmDialogueSlotObserver::HandleAudioChunk(
    const TArray<uint8>& AudioBytes, int64 /*TotalBytesReceived*/)
{
    if (UInoAgentsLiteRtLmDialogueQueue* Q = QueueWeak.Get())
    {
        Q->OnSlotChunk(SlotIndex, AudioBytes);
    }
}

void UInoAgentsLiteRtLmDialogueSlotObserver::HandleComplete(
    const TArray<uint8>& /*FullAudioBytes*/, EElevenLabsOutputFormat /*OutputFormat*/)
{
    if (UInoAgentsLiteRtLmDialogueQueue* Q = QueueWeak.Get())
    {
        Q->OnSlotComplete(SlotIndex);
    }
}

void UInoAgentsLiteRtLmDialogueSlotObserver::HandleError(FString ErrorMessage)
{
    if (UInoAgentsLiteRtLmDialogueQueue* Q = QueueWeak.Get())
    {
        Q->OnSlotError(SlotIndex, ErrorMessage);
    }
}

// ======================================================================
// Audio format derivation
// ======================================================================

namespace
{
    void DeriveAudioFormat(
        EElevenLabsOutputFormat ElevenLabsFmt,
        EInoAgentsAudioFormat&  OutFeedFormat,
        int32&                  OutPcmSampleRate)
    {
        switch (ElevenLabsFmt)
        {
            case EElevenLabsOutputFormat::Mp3_44100_128:
            case EElevenLabsOutputFormat::Mp3_44100_64:
            case EElevenLabsOutputFormat::Mp3_22050_32:
                OutFeedFormat    = EInoAgentsAudioFormat::Mp3;
                OutPcmSampleRate = 0;
                return;

            case EElevenLabsOutputFormat::Pcm_16000:
                OutFeedFormat    = EInoAgentsAudioFormat::PcmInt16;
                OutPcmSampleRate = 16000;
                return;

            case EElevenLabsOutputFormat::Pcm_24000:
                OutFeedFormat    = EInoAgentsAudioFormat::PcmInt16;
                OutPcmSampleRate = 24000;
                return;

            case EElevenLabsOutputFormat::Pcm_44100:
                OutFeedFormat    = EInoAgentsAudioFormat::PcmInt16;
                OutPcmSampleRate = 44100;
                return;

            default:
                OutFeedFormat    = EInoAgentsAudioFormat::Mp3;
                OutPcmSampleRate = 0;
                return;
        }
    }
}

// ======================================================================
// Queue — lifecycle
// ======================================================================

void UInoAgentsLiteRtLmDialogueQueue::Initialize(
    UObject*                             WorldContextObject,
    UInoAgentsStreamingAudioComponent*   InAudioComponent,
    ULiteRtLmConversation*               InConversation,
    const FString&                       InDefaultVoiceId,
    const FElevenLabsDialogueRequest&    InRequestTemplate,
    int32                                InDefaultPauseDurationMs)
{
    // Unbind from any prior conversation.
    Clear();

    WorldContextWeak        = WorldContextObject;
    AudioComponent          = InAudioComponent;
    DefaultVoiceId          = InDefaultVoiceId;
    RequestTemplate         = InRequestTemplate;
    DefaultPauseDurationMs  = FMath::Max(InDefaultPauseDurationMs, 0);

    // Derive audio feed format from the ElevenLabs output format.
    int32 PcmRate = 0;
    DeriveAudioFormat(RequestTemplate.OutputFormat, DerivedAudioFormat, PcmRate);

    if (DerivedAudioFormat == EInoAgentsAudioFormat::PcmInt16 && AudioComponent != nullptr)
    {
        AudioComponent->SetPcmFormat(PcmRate, /*NumChannels=*/1);
    }

    // Bind to the conversation's OnSentence and OnNewLine.
    BoundConversation = InConversation;
    if (InConversation != nullptr)
    {
        InConversation->OnSentence.AddDynamic(
            this, &UInoAgentsLiteRtLmDialogueQueue::HandleSentenceFromConversation);
        InConversation->OnNewLine.AddDynamic(
            this, &UInoAgentsLiteRtLmDialogueQueue::HandleNewLineFromConversation);
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsLiteRtLmDialogueQueue: initialized (voice=%s, model=%s, "
                "outputFmt=%d, audioFeedFmt=%d, pauseMs=%d, conversation=%s)"),
           *DefaultVoiceId,
           *RequestTemplate.ModelId,
           static_cast<int32>(RequestTemplate.OutputFormat),
           static_cast<int32>(DerivedAudioFormat),
           DefaultPauseDurationMs,
           InConversation ? *InConversation->GetName() : TEXT("none"));
}

void UInoAgentsLiteRtLmDialogueQueue::Clear()
{
    // Cancel any pending pause timer before resetting state — prevents
    // a stale callback from firing on the cleared queue.
    if (PauseTimerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(PauseTimerHandle);
        PauseTimerHandle.Reset();
    }

    // Unbind from the conversation if we're attached.
    if (ULiteRtLmConversation* Conv = BoundConversation.Get())
    {
        Conv->OnSentence.RemoveDynamic(
            this, &UInoAgentsLiteRtLmDialogueQueue::HandleSentenceFromConversation);
        Conv->OnNewLine.RemoveDynamic(
            this, &UInoAgentsLiteRtLmDialogueQueue::HandleNewLineFromConversation);
    }
    BoundConversation = nullptr;

    Slots.Reset();
    Observers.Reset();
    CurrentPlayIndex      = 0;
    bCurrentSlotStreaming  = false;
    bPauseTimerPending    = false;

    if (AudioComponent != nullptr)
    {
        AudioComponent->StopAndReset();
    }
}

void UInoAgentsLiteRtLmDialogueQueue::StopAndReset()
{
    // Cancel any pending pause timer before resetting state.
    if (PauseTimerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(PauseTimerHandle);
        PauseTimerHandle.Reset();
    }

    // Same as Clear but keeps the conversation binding so the queue
    // continues to receive OnSentence/OnNewLine for the next response.
    Slots.Reset();
    Observers.Reset();
    CurrentPlayIndex      = 0;
    bCurrentSlotStreaming  = false;
    bPauseTimerPending    = false;

    if (AudioComponent != nullptr)
    {
        AudioComponent->StopAndReset();
    }
}

// ======================================================================
// Auto-bound conversation handlers
// ======================================================================

void UInoAgentsLiteRtLmDialogueQueue::HandleSentenceFromConversation(
    FString RawText, FString /*CleanText*/)
{
    // Send RawText (with [emotion]/[audio] tags intact) to ElevenLabs.
    // Tags are consumed as delivery instructions and not spoken aloud.
    EnqueueSentenceInternal(RawText);
}

void UInoAgentsLiteRtLmDialogueQueue::HandleNewLineFromConversation()
{
    EnqueuePauseInternal();
}

// ======================================================================
// Internal sentence / pause dispatch
// ======================================================================

void UInoAgentsLiteRtLmDialogueQueue::EnqueueSentenceInternal(const FString& SentenceText)
{
    if (SentenceText.IsEmpty())
    {
        return;
    }

    UObject* Ctx = WorldContextWeak.Get();
    if (Ctx == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsLiteRtLmDialogueQueue: world context is null"));
        return;
    }

    const int32 SlotIndex = Slots.Num();
    Slots.AddDefaulted();

    const FString& VoiceId = DefaultVoiceId;

    FElevenLabsDialogueRequest Req = RequestTemplate;
    Req.Inputs.Reset();
    Req.Inputs.Add({ SentenceText, VoiceId });

    UElevenLabsTextToDialogueStream* Action =
        UElevenLabsTextToDialogueStream::StreamTextToDialogue(
            Ctx, Req, /*ApiKeyOverride=*/FString());
    if (Action == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsLiteRtLmDialogueQueue: StreamTextToDialogue returned null for slot %d"),
               SlotIndex);
        Slots[SlotIndex].bErrored  = true;
        Slots[SlotIndex].bComplete = true;
        DrainReadySlots();
        return;
    }

    UInoAgentsLiteRtLmDialogueSlotObserver* Observer = NewObject<UInoAgentsLiteRtLmDialogueSlotObserver>(this);
    Observer->SlotIndex = SlotIndex;
    Observer->QueueWeak = this;
    Observers.Add(Observer);

    Action->OnAudioChunk.AddDynamic(
        Observer, &UInoAgentsLiteRtLmDialogueSlotObserver::HandleAudioChunk);
    Action->OnComplete.AddDynamic(
        Observer, &UInoAgentsLiteRtLmDialogueSlotObserver::HandleComplete);
    Action->OnError.AddDynamic(
        Observer, &UInoAgentsLiteRtLmDialogueSlotObserver::HandleError);

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsLiteRtLmDialogueQueue: slot %d dispatched (\"%s\")"),
           SlotIndex, *SentenceText.Left(60));

    Action->Activate();
}

void UInoAgentsLiteRtLmDialogueQueue::EnqueuePauseInternal()
{
    if (DefaultPauseDurationMs <= 0)
    {
        return;
    }

    FSlot& Slot          = Slots.AddDefaulted_GetRef();
    Slot.bIsPause        = true;
    Slot.PauseDurationMs = DefaultPauseDurationMs;
    Slot.bComplete       = true;

    UE_LOG(LogInoAgents, Verbose,
           TEXT("UInoAgentsLiteRtLmDialogueQueue: slot %d is a %d-ms pause"),
           Slots.Num() - 1, DefaultPauseDurationMs);

    DrainReadySlots();
}

// ======================================================================
// Slot callbacks
// ======================================================================

void UInoAgentsLiteRtLmDialogueQueue::OnSlotChunk(
    int32 SlotIndex, const TArray<uint8>& Bytes)
{
    if (!Slots.IsValidIndex(SlotIndex))
    {
        return;
    }

    if (SlotIndex == CurrentPlayIndex)
    {
        if (AudioComponent != nullptr)
        {
            AudioComponent->FeedAudioBytes(Bytes, DerivedAudioFormat);
            bCurrentSlotStreaming = true;
        }
    }
    else
    {
        Slots[SlotIndex].BufferedBytes.Append(Bytes);
    }
}

void UInoAgentsLiteRtLmDialogueQueue::OnSlotComplete(int32 SlotIndex)
{
    if (!Slots.IsValidIndex(SlotIndex))
    {
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsLiteRtLmDialogueQueue: slot %d complete (%d buffered bytes)"),
           SlotIndex, Slots[SlotIndex].BufferedBytes.Num());

    Slots[SlotIndex].bComplete = true;
    DrainReadySlots();
}

void UInoAgentsLiteRtLmDialogueQueue::OnSlotError(int32 SlotIndex, const FString& ErrorMessage)
{
    if (!Slots.IsValidIndex(SlotIndex))
    {
        return;
    }

    UE_LOG(LogInoAgents, Warning,
           TEXT("UInoAgentsLiteRtLmDialogueQueue: slot %d error: %s"),
           SlotIndex, *ErrorMessage);

    Slots[SlotIndex].bErrored  = true;
    Slots[SlotIndex].bComplete = true;
    DrainReadySlots();
}

// ======================================================================
// Drain — advance past completed slots, feeding buffered audio
// ======================================================================

void UInoAgentsLiteRtLmDialogueQueue::DrainReadySlots()
{
    // If a pause timer is counting down, don't advance — the timer
    // callback will clear the flag and re-enter DrainReadySlots when
    // the pause is over. Without this guard, a concurrent
    // OnSlotComplete re-enters drain, advances past the consumed
    // pause slot (PauseDurationMs==0), and then the timer fires and
    // advances AGAIN — double-advancing, skipping a slot.
    if (bPauseTimerPending)
    {
        return;
    }

    while (Slots.IsValidIndex(CurrentPlayIndex))
    {
        FSlot& Slot = Slots[CurrentPlayIndex];

        if (!Slot.bComplete)
        {
            break;
        }

        // --- Pause slot: wait, then advance --------------------------
        if (Slot.bIsPause)
        {
            if (Slot.PauseDurationMs <= 0)
            {
                // Zero-duration pause — skip immediately.
                CurrentPlayIndex++;
                bCurrentSlotStreaming = false;
                continue;
            }

            const float DelaySec =
                static_cast<float>(Slot.PauseDurationMs) / 1000.0f;

            bPauseTimerPending = true;

            TWeakObjectPtr<UInoAgentsLiteRtLmDialogueQueue> WeakSelf(this);
            PauseTimerHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda(
                    [WeakSelf](float) -> bool
                    {
                        if (UInoAgentsLiteRtLmDialogueQueue* Self = WeakSelf.Get())
                        {
                            Self->PauseTimerHandle.Reset();
                            Self->bPauseTimerPending = false;
                            Self->CurrentPlayIndex++;
                            Self->bCurrentSlotStreaming = false;
                            Self->DrainReadySlots();
                        }
                        return false;
                    }),
                DelaySec);
            return;
        }

        // --- Audio slot: flush buffered bytes if any -----------------
        if (!Slot.bErrored && Slot.BufferedBytes.Num() > 0 && AudioComponent != nullptr)
        {
            AudioComponent->FeedAudioBytes(Slot.BufferedBytes, DerivedAudioFormat);
        }

        Slot.BufferedBytes.Reset();
        CurrentPlayIndex++;
        bCurrentSlotStreaming = false;

        UE_LOG(LogInoAgents, Verbose,
               TEXT("UInoAgentsLiteRtLmDialogueQueue: advanced to slot %d"),
               CurrentPlayIndex);
    }

    if (CurrentPlayIndex >= Slots.Num() && Slots.Num() > 0)
    {
        if (AudioComponent != nullptr)
        {
            AudioComponent->FinalizeStream();
        }

        UE_LOG(LogInoAgents, Log,
               TEXT("UInoAgentsLiteRtLmDialogueQueue: all %d slot(s) played"),
               Slots.Num());

        OnAllComplete.Broadcast();
    }
}
