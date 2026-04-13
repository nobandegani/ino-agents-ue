// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoLiteRtLmDialogueQueue.h"

#include "ElevenLabs/InoElevenLabsTextToDialogueStream.h"
#include "InoAgentsLog.h"
#include "LiteRtLm/InoLiteRtLmConversation.h"

// RuntimeAudioImporter plugin — provides the streaming sound wave +
// raw/encoded format enums.
#include "RuntimeAudioImporterTypes.h"
#include "Sound/StreamingSoundWave.h"

// ======================================================================
// Slot observer — per-TTS-action trampoline
// ======================================================================

void UInoLiteRtLmDialogueSlotObserver::HandleAudioChunk(
    const TArray<uint8>& AudioBytes, int64 /*TotalBytesReceived*/)
{
    if (UInoLiteRtLmDialogueQueue* Q = QueueWeak.Get())
    {
        Q->OnSlotChunk(SlotIndex, AudioBytes);
    }
}

void UInoLiteRtLmDialogueSlotObserver::HandleComplete(
    const TArray<uint8>& /*FullAudioBytes*/, EInoElevenLabsOutputFormat /*OutputFormat*/)
{
    if (UInoLiteRtLmDialogueQueue* Q = QueueWeak.Get())
    {
        Q->OnSlotComplete(SlotIndex);
    }
}

void UInoLiteRtLmDialogueSlotObserver::HandleError(FString ErrorMessage)
{
    if (UInoLiteRtLmDialogueQueue* Q = QueueWeak.Get())
    {
        Q->OnSlotError(SlotIndex, ErrorMessage);
    }
}

// ======================================================================
// Audio format derivation
// ======================================================================

namespace
{
    /**
     * Translate an ElevenLabs output format into the queue's feed
     * format (MP3 vs. RAW PCM int16) plus its sample rate. The rate
     * is needed for both feed paths — RAW so we can pass it to
     * AppendAudioDataFromRAW, MP3 so we can inject silence at the
     * matching rate during pause slots.
     */
    void DeriveAudioFormat(
        EInoElevenLabsOutputFormat ElevenLabsFmt,
        bool&                      OutIsMP3,
        int32&                     OutSampleRate)
    {
        switch (ElevenLabsFmt)
        {
            case EInoElevenLabsOutputFormat::Mp3_44100_128:
            case EInoElevenLabsOutputFormat::Mp3_44100_64:
                OutIsMP3      = true;
                OutSampleRate = 44100;
                return;

            case EInoElevenLabsOutputFormat::Mp3_22050_32:
                OutIsMP3      = true;
                OutSampleRate = 22050;
                return;

            case EInoElevenLabsOutputFormat::Pcm_16000:
                OutIsMP3      = false;
                OutSampleRate = 16000;
                return;

            case EInoElevenLabsOutputFormat::Pcm_24000:
                OutIsMP3      = false;
                OutSampleRate = 24000;
                return;

            case EInoElevenLabsOutputFormat::Pcm_44100:
                OutIsMP3      = false;
                OutSampleRate = 44100;
                return;

            default:
                OutIsMP3      = true;
                OutSampleRate = 44100;
                return;
        }
    }
}

// ======================================================================
// Queue — lifecycle
// ======================================================================

void UInoLiteRtLmDialogueQueue::Initialize(
    UObject*                              WorldContextObject,
    UStreamingSoundWave*                  InStreamingWave,
    UInoLiteRtLmConversation*             InConversation,
    const FString&                        InDefaultVoiceId,
    const FInoElevenLabsDialogueRequest&  InRequestTemplate,
    int32                                 InDefaultPauseDurationMs)
{
    Clear();

    WorldContextWeak        = WorldContextObject;
    StreamingWave           = InStreamingWave;
    DefaultVoiceId          = InDefaultVoiceId;
    RequestTemplate         = InRequestTemplate;
    DefaultPauseDurationMs  = FMath::Max(InDefaultPauseDurationMs, 0);

    DerivedNumChannels = 1;
    DeriveAudioFormat(RequestTemplate.OutputFormat,
                      bDerivedFormatIsMP3,
                      DerivedSampleRate);

    // Pre-configure the wave's desired rate + channels for both
    // modes. MP3 will reconfirm on the first decoded frame; PCM
    // accepts immediately.
    if (StreamingWave != nullptr && DerivedSampleRate > 0)
    {
        StreamingWave->SetInitialDesiredSampleRate(DerivedSampleRate);
        StreamingWave->SetInitialDesiredNumOfChannels(DerivedNumChannels);
    }

    BoundConversation = InConversation;
    if (InConversation != nullptr)
    {
        InConversation->OnSentence.AddDynamic(
            this, &UInoLiteRtLmDialogueQueue::HandleSentenceFromConversation);
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoLiteRtLmDialogueQueue: initialized (voice=%s, model=%s, "
                "outputFmt=%d, feedIsMP3=%d, rate=%d Hz, pauseMs=%d, conversation=%s)"),
           *DefaultVoiceId,
           *RequestTemplate.ModelId,
           static_cast<int32>(RequestTemplate.OutputFormat),
           bDerivedFormatIsMP3 ? 1 : 0,
           DerivedSampleRate,
           DefaultPauseDurationMs,
           InConversation ? *InConversation->GetName() : TEXT("none"));
}

void UInoLiteRtLmDialogueQueue::Clear()
{
    const bool bWasInFlight = (Slots.Num() > 0 && !bAllCompleteBroadcasted);

    if (UInoLiteRtLmConversation* Conv = BoundConversation.Get())
    {
        Conv->OnSentence.RemoveDynamic(
            this, &UInoLiteRtLmDialogueQueue::HandleSentenceFromConversation);
    }
    BoundConversation = nullptr;

    Slots.Reset();
    Observers.Reset();
    CurrentPlayIndex         = 0;
    bAllCompleteBroadcasted  = false;

    // Reset the wave so stale audio from the prior cycle doesn't leak
    // into the next one. Turning the drain flag off prevents the
    // first OnAudioPlaybackFinished from firing before any real data
    // lands in the next cycle.
    if (StreamingWave != nullptr)
    {
        StreamingWave->SetStopSoundOnPlaybackFinish(false);
        StreamingWave->ReleaseMemory();
    }

    if (bWasInFlight)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("UInoLiteRtLmDialogueQueue: interrupted mid-cycle (Clear)"));
        OnAudioInterrupted.Broadcast();
    }
}

void UInoLiteRtLmDialogueQueue::StopAndReset()
{
    const bool bWasInFlight = (Slots.Num() > 0 && !bAllCompleteBroadcasted);

    Slots.Reset();
    Observers.Reset();
    CurrentPlayIndex         = 0;
    bAllCompleteBroadcasted  = false;

    if (StreamingWave != nullptr)
    {
        StreamingWave->SetStopSoundOnPlaybackFinish(false);
        StreamingWave->ReleaseMemory();
    }

    if (bWasInFlight)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("UInoLiteRtLmDialogueQueue: interrupted mid-cycle (StopAndReset)"));
        OnAudioInterrupted.Broadcast();
    }
}

void UInoLiteRtLmDialogueQueue::SetPauseDurationMs(int32 InDurationMs)
{
    DefaultPauseDurationMs = FMath::Max(InDurationMs, 0);
}

// ======================================================================
// Conversation handler
// ======================================================================

void UInoLiteRtLmDialogueQueue::HandleSentenceFromConversation(
    FString RawText, FString /*CleanText*/)
{
    // Send RawText with [emotion] tags intact (ElevenLabs uses them as
    // delivery hints) but strip {curly} emotion-state tags which would
    // be spoken aloud literally.
    FString TtsText;
    TtsText.Reserve(RawText.Len());
    int32 CurlyDepth = 0;
    for (const TCHAR Ch : RawText)
    {
        if (Ch == TEXT('{')) { CurlyDepth++; continue; }
        if (Ch == TEXT('}') && CurlyDepth > 0) { CurlyDepth--; continue; }
        if (CurlyDepth == 0) { TtsText.AppendChar(Ch); }
    }
    TtsText.TrimStartAndEndInline();

    if (TtsText.IsEmpty())
    {
        return;
    }

    EnqueueSentenceInternal(TtsText);
}

// ======================================================================
// Internal sentence / pause dispatch
// ======================================================================

void UInoLiteRtLmDialogueQueue::EnqueueSentenceInternal(const FString& SentenceText)
{
    if (SentenceText.IsEmpty())
    {
        return;
    }

    UObject* Ctx = WorldContextWeak.Get();
    if (Ctx == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoLiteRtLmDialogueQueue: world context is null"));
        return;
    }

    // Insert a silence-pause slot before every sentence except the first.
    if (DefaultPauseDurationMs > 0
        && Slots.Num() > 0
        && !Slots.Last().bIsPause)
    {
        EnqueuePauseInternal();
    }

    const int32 SlotIndex = Slots.Num();
    Slots.AddDefaulted();
    bAllCompleteBroadcasted = false;

    const FString& VoiceId = DefaultVoiceId;

    FInoElevenLabsDialogueRequest Req = RequestTemplate;
    Req.Inputs.Reset();
    Req.Inputs.Add({ SentenceText, VoiceId });

    UInoElevenLabsTextToDialogueStream* Action =
        UInoElevenLabsTextToDialogueStream::StreamTextToDialogue(
            Ctx, Req, /*ApiKeyOverride=*/FString());
    if (Action == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoLiteRtLmDialogueQueue: StreamTextToDialogue returned null for slot %d"),
               SlotIndex);
        Slots[SlotIndex].bErrored  = true;
        Slots[SlotIndex].bComplete = true;
        DrainReadySlots();
        return;
    }

    UInoLiteRtLmDialogueSlotObserver* Observer =
        NewObject<UInoLiteRtLmDialogueSlotObserver>(this);
    Observer->SlotIndex = SlotIndex;
    Observer->QueueWeak = this;
    Observers.Add(Observer);

    Action->OnAudioChunk.AddDynamic(
        Observer, &UInoLiteRtLmDialogueSlotObserver::HandleAudioChunk);
    Action->OnComplete.AddDynamic(
        Observer, &UInoLiteRtLmDialogueSlotObserver::HandleComplete);
    Action->OnError.AddDynamic(
        Observer, &UInoLiteRtLmDialogueSlotObserver::HandleError);

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoLiteRtLmDialogueQueue: slot %d dispatched (\"%s\")"),
           SlotIndex, *SentenceText.Left(60));

    Action->Activate();

    DrainReadySlots();
}

void UInoLiteRtLmDialogueQueue::EnqueuePauseInternal()
{
    if (DefaultPauseDurationMs <= 0)
    {
        return;
    }

    FSlot& Slot          = Slots.AddDefaulted_GetRef();
    Slot.bIsPause        = true;
    Slot.PauseDurationMs = DefaultPauseDurationMs;
    Slot.bComplete       = true;
    bAllCompleteBroadcasted = false;

    UE_LOG(LogInoAgents, Verbose,
           TEXT("UInoLiteRtLmDialogueQueue: slot %d is a %d-ms silence"),
           Slots.Num() - 1, DefaultPauseDurationMs);
}

// ======================================================================
// Wave feed helpers
// ======================================================================

void UInoLiteRtLmDialogueQueue::FeedBytesToWave(const TArray<uint8>& Bytes)
{
    if (StreamingWave == nullptr || Bytes.Num() == 0)
    {
        return;
    }

    if (bDerivedFormatIsMP3)
    {
        // RuntimeAudio's append takes TArray<uint8> by value (move-friendly).
        // Copy here since the caller may reuse Bytes after this returns.
        StreamingWave->AppendAudioDataFromEncoded(
            TArray<uint8>(Bytes), ERuntimeAudioFormat::Mp3);
    }
    else
    {
        StreamingWave->AppendAudioDataFromRAW(
            TArray<uint8>(Bytes),
            ERuntimeRAWAudioFormat::Int16,
            DerivedSampleRate,
            DerivedNumChannels);
    }
}

void UInoLiteRtLmDialogueQueue::InjectSilence(int32 PauseMs)
{
    if (StreamingWave == nullptr || PauseMs <= 0 || DerivedSampleRate <= 0)
    {
        return;
    }

    const int32 Channels    = FMath::Max(DerivedNumChannels, 1);
    const int64 NumFrames   =
        (static_cast<int64>(DerivedSampleRate) * static_cast<int64>(PauseMs)) / 1000;
    const int64 NumSamples  = NumFrames * Channels;
    const int64 NumBytes    = NumSamples * static_cast<int64>(sizeof(int16));
    if (NumBytes <= 0 || NumBytes > MAX_int32)
    {
        return;
    }

    TArray<uint8> Silence;
    Silence.SetNumZeroed(static_cast<int32>(NumBytes));

    StreamingWave->AppendAudioDataFromRAW(
        MoveTemp(Silence),
        ERuntimeRAWAudioFormat::Int16,
        DerivedSampleRate,
        Channels);

    UE_LOG(LogInoAgents, Verbose,
           TEXT("UInoLiteRtLmDialogueQueue: injected %d frames of silence (%d ms)"),
           static_cast<int32>(NumFrames), PauseMs);
}

// ======================================================================
// Slot callbacks
// ======================================================================

void UInoLiteRtLmDialogueQueue::OnSlotChunk(
    int32 SlotIndex, const TArray<uint8>& Bytes)
{
    if (!Slots.IsValidIndex(SlotIndex))
    {
        return;
    }

    FSlot& Slot = Slots[SlotIndex];

    if (SlotIndex == CurrentPlayIndex)
    {
        // Flush any bytes that accumulated while this slot wasn't current
        // first, so byte order stays correct.
        if (Slot.BufferedBytes.Num() > 0)
        {
            FeedBytesToWave(Slot.BufferedBytes);
            Slot.BufferedBytes.Reset();
        }
        FeedBytesToWave(Bytes);
    }
    else
    {
        Slot.BufferedBytes.Append(Bytes);
    }
}

void UInoLiteRtLmDialogueQueue::OnSlotComplete(int32 SlotIndex)
{
    if (!Slots.IsValidIndex(SlotIndex))
    {
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoLiteRtLmDialogueQueue: slot %d complete (%d buffered bytes)"),
           SlotIndex, Slots[SlotIndex].BufferedBytes.Num());

    Slots[SlotIndex].bComplete = true;
    DrainReadySlots();
}

void UInoLiteRtLmDialogueQueue::OnSlotError(int32 SlotIndex, const FString& ErrorMessage)
{
    if (!Slots.IsValidIndex(SlotIndex))
    {
        return;
    }

    UE_LOG(LogInoAgents, Warning,
           TEXT("UInoLiteRtLmDialogueQueue: slot %d error: %s"),
           SlotIndex, *ErrorMessage);

    Slots[SlotIndex].bErrored  = true;
    Slots[SlotIndex].bComplete = true;
    DrainReadySlots();
}

// ======================================================================
// Drain — advance past completed slots, feeding buffered audio
// ======================================================================

void UInoLiteRtLmDialogueQueue::DrainReadySlots()
{
    while (Slots.IsValidIndex(CurrentPlayIndex))
    {
        FSlot& Slot = Slots[CurrentPlayIndex];

        // Flush any bytes that accumulated while this slot wasn't
        // current. Critical for byte ordering: without this, late
        // chunks for a slot whose CurrentPlayIndex moved past it
        // would never play.
        if (!Slot.bIsPause
            && !Slot.bErrored
            && Slot.BufferedBytes.Num() > 0)
        {
            FeedBytesToWave(Slot.BufferedBytes);
            Slot.BufferedBytes.Reset();
        }

        if (!Slot.bComplete)
        {
            break;
        }

        if (Slot.bIsPause)
        {
            InjectSilence(Slot.PauseDurationMs);
            CurrentPlayIndex++;
            continue;
        }

        CurrentPlayIndex++;

        UE_LOG(LogInoAgents, Verbose,
               TEXT("UInoLiteRtLmDialogueQueue: advanced to slot %d"),
               CurrentPlayIndex);
    }

    if (CurrentPlayIndex >= Slots.Num()
        && Slots.Num() > 0
        && !bAllCompleteBroadcasted)
    {
        // All slots dispatched. Flip the wave into drain mode so
        // OnAudioPlaybackFinished fires when the buffer empties and
        // the audio component naturally stops.
        if (StreamingWave != nullptr)
        {
            StreamingWave->SetStopSoundOnPlaybackFinish(true);
        }

        bAllCompleteBroadcasted = true;

        UE_LOG(LogInoAgents, Log,
               TEXT("UInoLiteRtLmDialogueQueue: all %d slot(s) dispatched"),
               Slots.Num());

        OnAllComplete.Broadcast();
    }
}
