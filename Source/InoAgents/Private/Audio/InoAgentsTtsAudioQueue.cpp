// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoAgentsTtsAudioQueue.h"

#include "Audio/InoAgentsStreamingAudioComponent.h"
#include "ElevenLabs/ElevenLabsTextToDialogueStream.h"
#include "InoAgentsLog.h"

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
    // We don't use FullAudioBytes here — we've already accumulated
    // everything via HandleAudioChunk. This callback just signals
    // "slot is done".
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
// Queue
// ======================================================================

namespace
{
    /** Map an ElevenLabs output format to the audio component's feed
     *  format and PCM sample rate (ignored for MP3). */
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
                OutPcmSampleRate = 0;  // unused for MP3
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

            case EElevenLabsOutputFormat::Ulaw_8000:
                // u-law is not decoded by the audio component today;
                // fall back to MP3 and let the caller notice the
                // mismatch. A future phase can add u-law decoding.
                OutFeedFormat    = EInoAgentsAudioFormat::Mp3;
                OutPcmSampleRate = 0;
                return;

            default:
                OutFeedFormat    = EInoAgentsAudioFormat::Mp3;
                OutPcmSampleRate = 0;
                return;
        }
    }
}

void UInoAgentsTtsAudioQueue::Initialize(
    UObject*                             WorldContextObject,
    UInoAgentsStreamingAudioComponent*   InAudioComponent,
    const FString&                       InDefaultVoiceId,
    const FElevenLabsDialogueRequest&    InRequestTemplate,
    int32                                InDefaultPauseDurationMs)
{
    WorldContextWeak        = WorldContextObject;
    AudioComponent          = InAudioComponent;
    DefaultVoiceId          = InDefaultVoiceId;
    RequestTemplate         = InRequestTemplate;
    DefaultPauseDurationMs  = FMath::Max(InDefaultPauseDurationMs, 0);
    CurrentPlayIndex        = 0;
    bCurrentSlotStreaming    = false;
    Slots.Reset();
    Observers.Reset();

    // Derive the audio format + PCM sample rate from the ElevenLabs
    // output format so the queue feeds the right bytes to the audio
    // component without the caller having to specify it twice.
    int32 PcmRate = 0;
    DeriveAudioFormat(RequestTemplate.OutputFormat, DerivedAudioFormat, PcmRate);

    if (DerivedAudioFormat == EInoAgentsAudioFormat::PcmInt16 && AudioComponent != nullptr)
    {
        AudioComponent->SetPcmFormat(PcmRate, /*NumChannels=*/1);
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsTtsAudioQueue: initialized (voice=%s, model=%s, "
                "outputFmt=%d, audioFeedFmt=%d)"),
           *DefaultVoiceId,
           *RequestTemplate.ModelId,
           static_cast<int32>(RequestTemplate.OutputFormat),
           static_cast<int32>(DerivedAudioFormat));
}

void UInoAgentsTtsAudioQueue::EnqueueSentence(
    const FString& SentenceText,
    const FString& VoiceIdOverride)
{
    if (SentenceText.IsEmpty())
    {
        return;
    }

    UObject* Ctx = WorldContextWeak.Get();
    if (Ctx == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsTtsAudioQueue::EnqueueSentence: world context is null"));
        return;
    }

    // Allocate a slot.
    const int32 SlotIndex = Slots.Num();
    Slots.AddDefaulted();

    // Build the TTS request from the stored template — copy all
    // settings (ModelId, OutputFormat, Stability, Seed, etc.) and
    // replace only the Inputs array with this sentence's text + voice.
    const FString& VoiceId = VoiceIdOverride.IsEmpty() ? DefaultVoiceId : VoiceIdOverride;

    FElevenLabsDialogueRequest Req = RequestTemplate;
    Req.Inputs.Reset();
    Req.Inputs.Add({ SentenceText, VoiceId });

    // Create the async action.
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

    // Create a per-slot observer so the delegate trampoline knows
    // which slot's bytes are arriving. Held alive by our Observers
    // UPROPERTY array.
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
           TEXT("UInoAgentsTtsAudioQueue: slot %d dispatched (\"%s\", voice=%s)"),
           SlotIndex,
           *SentenceText.Left(40),
           *VoiceId);

    Action->Activate();
}

void UInoAgentsTtsAudioQueue::EnqueuePause()
{
    if (DefaultPauseDurationMs <= 0)
    {
        return;  // 0 ms pause = skip entirely
    }

    FSlot& Slot        = Slots.AddDefaulted_GetRef();
    Slot.bIsPause       = true;
    Slot.PauseDurationMs = DefaultPauseDurationMs;
    Slot.bComplete      = true;  // pause slots are instantly "complete"

    UE_LOG(LogInoAgents, Verbose,
           TEXT("UInoAgentsTtsAudioQueue: slot %d is a %d-ms pause"),
           Slots.Num() - 1, DefaultPauseDurationMs);

    DrainReadySlots();
}

void UInoAgentsTtsAudioQueue::Clear()
{
    Slots.Reset();
    Observers.Reset();
    CurrentPlayIndex      = 0;
    bCurrentSlotStreaming  = false;

    if (AudioComponent != nullptr)
    {
        AudioComponent->StopAndReset();
    }
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
        // This is the head-of-line slot — stream its bytes directly
        // to the audio component for minimum latency. No buffering.
        if (AudioComponent != nullptr)
        {
            AudioComponent->FeedAudioBytes(Bytes, DerivedAudioFormat);
            bCurrentSlotStreaming = true;
        }
    }
    else
    {
        // Future slot — buffer until it becomes the current one.
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
    while (Slots.IsValidIndex(CurrentPlayIndex))
    {
        FSlot& Slot = Slots[CurrentPlayIndex];

        if (!Slot.bComplete)
        {
            // Current slot is still receiving bytes from ElevenLabs.
            // Nothing to advance past — we'll come back when
            // OnSlotComplete fires for this slot.
            break;
        }

        // --- Pause slot: wait, then advance --------------------------
        if (Slot.bIsPause && Slot.PauseDurationMs > 0)
        {
            // Mark the pause as "consumed" so we don't re-enter it
            // when the timer fires and calls DrainReadySlots again.
            Slot.PauseDurationMs = 0;

            const float DelaySec =
                static_cast<float>(Slots[CurrentPlayIndex].PauseDurationMs > 0
                    ? Slots[CurrentPlayIndex].PauseDurationMs
                    : DefaultPauseDurationMs) / 1000.0f;

            TWeakObjectPtr<UInoAgentsTtsAudioQueue> WeakSelf(this);
            FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda(
                    [WeakSelf](float) -> bool
                    {
                        if (UInoAgentsTtsAudioQueue* Self = WeakSelf.Get())
                        {
                            Self->CurrentPlayIndex++;
                            Self->bCurrentSlotStreaming = false;
                            Self->DrainReadySlots();
                        }
                        return false;  // one-shot
                    }),
                DelaySec);
            return;  // stop draining until the timer fires
        }

        // --- Audio slot: flush buffered bytes if any -----------------
        if (!Slot.bErrored && Slot.BufferedBytes.Num() > 0 && AudioComponent != nullptr)
        {
            AudioComponent->FeedAudioBytes(Slot.BufferedBytes, DerivedAudioFormat);
        }

        // Free the buffer — bytes are in the audio component now.
        Slot.BufferedBytes.Reset();

        // Advance to the next slot.
        CurrentPlayIndex++;
        bCurrentSlotStreaming = false;

        UE_LOG(LogInoAgents, Verbose,
               TEXT("UInoAgentsTtsAudioQueue: advanced to slot %d"),
               CurrentPlayIndex);
    }

    // If we've played every slot, finalize the audio stream and
    // broadcast OnAllComplete.
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
