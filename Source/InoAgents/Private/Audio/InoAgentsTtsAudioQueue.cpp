// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoAgentsTtsAudioQueue.h"

#include "Audio/InoAgentsStreamingAudioComponent.h"
#include "ElevenLabs/ElevenLabsTextToDialogueStream.h"
#include "InoAgentsLog.h"

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

void UInoAgentsTtsAudioQueue::Initialize(
    UObject*                             WorldContextObject,
    UInoAgentsStreamingAudioComponent*   InAudioComponent,
    const FString&                       InDefaultVoiceId)
{
    WorldContextWeak = WorldContextObject;
    AudioComponent   = InAudioComponent;
    DefaultVoiceId   = InDefaultVoiceId;
    CurrentPlayIndex = 0;
    bCurrentSlotStreaming = false;
    Slots.Reset();
    Observers.Reset();
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

    // Build the TTS request — one input per sentence.
    const FString& VoiceId = VoiceIdOverride.IsEmpty() ? DefaultVoiceId : VoiceIdOverride;

    FElevenLabsDialogueRequest Req;
    Req.Inputs.Add({ SentenceText, VoiceId });
    // Leave ModelId / OutputFormat / etc. at their defaults — the
    // subsystem fills in the DefaultModelId and the user's Project
    // Settings output format.

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
            AudioComponent->FeedAudioBytes(Bytes, EInoAgentsAudioFormat::Mp3);
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

        // If the current slot had been streaming directly (head-of-line
        // path in OnSlotChunk), its bytes are already in the audio
        // component's queue. If it was a FUTURE slot that completed
        // before reaching head-of-line, flush its buffered bytes now.
        if (!Slot.bErrored && Slot.BufferedBytes.Num() > 0 && AudioComponent != nullptr)
        {
            AudioComponent->FeedAudioBytes(Slot.BufferedBytes, EInoAgentsAudioFormat::Mp3);
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
