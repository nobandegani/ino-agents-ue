// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoAgentsTtsAudioQueue.h"

#include "Audio/InoAgentsStreamingAudioComponent.h"
#include "ElevenLabs/ElevenLabsTextToDialogueStream.h"
#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmConversation.h"

#include "Containers/Ticker.h"

// ======================================================================
// Slot observer — per-TTS-action trampoline
// ======================================================================

void UInoAgentsTtsSlotObserver::HandleAudioChunk(
    const TArray<uint8>& AudioBytes, int64 /*TotalBytesReceived*/)
{
    if (UInoAgentsTtsAudioQueue* Q = QueueWeak.Get())
    {
        Q->OnSlotChunk(SlotIndex, AudioBytes);
    }
}

void UInoAgentsTtsSlotObserver::HandleComplete(
    const TArray<uint8>& /*FullAudioBytes*/, EElevenLabsOutputFormat /*OutputFormat*/)
{
    if (UInoAgentsTtsAudioQueue* Q = QueueWeak.Get())
    {
        Q->OnSlotComplete(SlotIndex);
    }
}

void UInoAgentsTtsSlotObserver::HandleError(FString ErrorMessage)
{
    if (UInoAgentsTtsAudioQueue* Q = QueueWeak.Get())
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

void UInoAgentsTtsAudioQueue::Initialize(
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
            this, &UInoAgentsTtsAudioQueue::HandleSentenceFromConversation);
        InConversation->OnNewLine.AddDynamic(
            this, &UInoAgentsTtsAudioQueue::HandleNewLineFromConversation);
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsTtsAudioQueue: initialized (voice=%s, model=%s, "
                "outputFmt=%d, audioFeedFmt=%d, pauseMs=%d, conversation=%s)"),
           *DefaultVoiceId,
           *RequestTemplate.ModelId,
           static_cast<int32>(RequestTemplate.OutputFormat),
           static_cast<int32>(DerivedAudioFormat),
           DefaultPauseDurationMs,
           InConversation ? *InConversation->GetName() : TEXT("none"));
}

void UInoAgentsTtsAudioQueue::Clear()
{
    // Unbind from the conversation if we're attached.
    if (ULiteRtLmConversation* Conv = BoundConversation.Get())
    {
        Conv->OnSentence.RemoveDynamic(
            this, &UInoAgentsTtsAudioQueue::HandleSentenceFromConversation);
        Conv->OnNewLine.RemoveDynamic(
            this, &UInoAgentsTtsAudioQueue::HandleNewLineFromConversation);
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

// ======================================================================
// Auto-bound conversation handlers
// ======================================================================

void UInoAgentsTtsAudioQueue::HandleSentenceFromConversation(FString SentenceText)
{
    EnqueueSentenceInternal(SentenceText);
}

void UInoAgentsTtsAudioQueue::HandleNewLineFromConversation()
{
    EnqueuePauseInternal();
}

// ======================================================================
// Internal sentence / pause dispatch
// ======================================================================

void UInoAgentsTtsAudioQueue::EnqueueSentenceInternal(const FString& SentenceText)
{
    if (SentenceText.IsEmpty())
    {
        return;
    }

    UObject* Ctx = WorldContextWeak.Get();
    if (Ctx == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsTtsAudioQueue: world context is null"));
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
               TEXT("UInoAgentsTtsAudioQueue: StreamTextToDialogue returned null for slot %d"),
               SlotIndex);
        Slots[SlotIndex].bErrored  = true;
        Slots[SlotIndex].bComplete = true;
        DrainReadySlots();
        return;
    }

    UInoAgentsTtsSlotObserver* Observer = NewObject<UInoAgentsTtsSlotObserver>(this);
    Observer->SlotIndex = SlotIndex;
    Observer->QueueWeak = this;
    Observers.Add(Observer);

    Action->OnAudioChunk.AddDynamic(
        Observer, &UInoAgentsTtsSlotObserver::HandleAudioChunk);
    Action->OnComplete.AddDynamic(
        Observer, &UInoAgentsTtsSlotObserver::HandleComplete);
    Action->OnError.AddDynamic(
        Observer, &UInoAgentsTtsSlotObserver::HandleError);

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsTtsAudioQueue: slot %d dispatched (\"%s\")"),
           SlotIndex, *SentenceText.Left(60));

    Action->Activate();
}

void UInoAgentsTtsAudioQueue::EnqueuePauseInternal()
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
           TEXT("UInoAgentsTtsAudioQueue: slot %d is a %d-ms pause"),
           Slots.Num() - 1, DefaultPauseDurationMs);

    DrainReadySlots();
}

// ======================================================================
// Slot callbacks
// ======================================================================

void UInoAgentsTtsAudioQueue::OnSlotChunk(
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

void UInoAgentsTtsAudioQueue::OnSlotComplete(int32 SlotIndex)
{
    if (!Slots.IsValidIndex(SlotIndex))
    {
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsTtsAudioQueue: slot %d complete (%d buffered bytes)"),
           SlotIndex, Slots[SlotIndex].BufferedBytes.Num());

    Slots[SlotIndex].bComplete = true;
    DrainReadySlots();
}

void UInoAgentsTtsAudioQueue::OnSlotError(int32 SlotIndex, const FString& ErrorMessage)
{
    if (!Slots.IsValidIndex(SlotIndex))
    {
        return;
    }

    UE_LOG(LogInoAgents, Warning,
           TEXT("UInoAgentsTtsAudioQueue: slot %d error: %s"),
           SlotIndex, *ErrorMessage);

    Slots[SlotIndex].bErrored  = true;
    Slots[SlotIndex].bComplete = true;
    DrainReadySlots();
}

// ======================================================================
// Drain — advance past completed slots, feeding buffered audio
// ======================================================================

void UInoAgentsTtsAudioQueue::DrainReadySlots()
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

            TWeakObjectPtr<UInoAgentsTtsAudioQueue> WeakSelf(this);
            FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda(
                    [WeakSelf](float) -> bool
                    {
                        if (UInoAgentsTtsAudioQueue* Self = WeakSelf.Get())
                        {
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
               TEXT("UInoAgentsTtsAudioQueue: advanced to slot %d"),
               CurrentPlayIndex);
    }

    if (CurrentPlayIndex >= Slots.Num() && Slots.Num() > 0)
    {
        if (AudioComponent != nullptr)
        {
            AudioComponent->FinalizeStream();
        }

        UE_LOG(LogInoAgents, Log,
               TEXT("UInoAgentsTtsAudioQueue: all %d slot(s) played"),
               Slots.Num());

        OnAllComplete.Broadcast();
    }
}
