// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoAgentsLiteRtLmDialogueQueue.h"

#include "Audio/InoAgentsStreamingSoundWave.h"
#include "ElevenLabs/ElevenLabsTextToDialogueStream.h"
#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmConversation.h"

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
    /**
     * Translate an ElevenLabs output format into the feed path + rate
     * used on the streaming wave. MP3 formats take the
     * AppendAudioDataFromMP3 path; RAW PCM formats take the
     * AppendAudioDataFromRAW path with Int16 samples. The rate is
     * returned for BOTH paths because silence injection during pause
     * slots needs it even in MP3 mode (silence is always written as
     * Int16 zeros via AppendAudioDataFromRAW, matching the rate of
     * the surrounding MP3 frames).
     */
    void DeriveAudioFormat(
        EElevenLabsOutputFormat ElevenLabsFmt,
        bool&                   OutIsMP3,
        int32&                  OutSampleRate)
    {
        switch (ElevenLabsFmt)
        {
            case EElevenLabsOutputFormat::Mp3_44100_128:
            case EElevenLabsOutputFormat::Mp3_44100_64:
                OutIsMP3      = true;
                OutSampleRate = 44100;
                return;

            case EElevenLabsOutputFormat::Mp3_22050_32:
                OutIsMP3      = true;
                OutSampleRate = 22050;
                return;

            case EElevenLabsOutputFormat::Pcm_16000:
                OutIsMP3      = false;
                OutSampleRate = 16000;
                return;

            case EElevenLabsOutputFormat::Pcm_24000:
                OutIsMP3      = false;
                OutSampleRate = 24000;
                return;

            case EElevenLabsOutputFormat::Pcm_44100:
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

void UInoAgentsLiteRtLmDialogueQueue::Initialize(
    UObject*                             WorldContextObject,
    UInoAgentsStreamingSoundWave*        InStreamingWave,
    ULiteRtLmConversation*               InConversation,
    const FString&                       InDefaultVoiceId,
    const FElevenLabsDialogueRequest&    InRequestTemplate,
    int32                                InDefaultPauseDurationMs)
{
    // Unbind from any prior conversation.
    Clear();

    WorldContextWeak        = WorldContextObject;
    StreamingWave           = InStreamingWave;
    DefaultVoiceId          = InDefaultVoiceId;
    RequestTemplate         = InRequestTemplate;
    DefaultPauseDurationMs  = FMath::Max(InDefaultPauseDurationMs, 0);

    // Derive the audio format + rate up front. Both MP3 and PCM paths
    // have a definitive rate from the ElevenLabs request so silence
    // injection works for either.
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
        StreamingWave->SetInitialDesiredNumChannels(DerivedNumChannels);
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
                "outputFmt=%d, feedIsMP3=%d, rate=%d Hz, pauseMs=%d, conversation=%s)"),
           *DefaultVoiceId,
           *RequestTemplate.ModelId,
           static_cast<int32>(RequestTemplate.OutputFormat),
           bDerivedFormatIsMP3 ? 1 : 0,
           DerivedSampleRate,
           DefaultPauseDurationMs,
           InConversation ? *InConversation->GetName() : TEXT("none"));
}

void UInoAgentsLiteRtLmDialogueQueue::Clear()
{
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
    CurrentPlayIndex         = 0;
    bAllCompleteBroadcasted  = false;

    // Reset the wave's buffer so stale audio from the prior cycle
    // doesn't leak into the next one. Turn the drain flag off so a
    // future cycle doesn't spuriously fire OnAudioPlaybackFinished
    // before any real data lands.
    if (StreamingWave != nullptr)
    {
        StreamingWave->SetStopSoundOnPlaybackFinish(false);
        StreamingWave->ResetStreamingBuffer();
    }
}

void UInoAgentsLiteRtLmDialogueQueue::StopAndReset()
{
    // Same as Clear but keeps the conversation binding so the queue
    // continues to receive OnSentence/OnNewLine for the next response.
    Slots.Reset();
    Observers.Reset();
    CurrentPlayIndex         = 0;
    bAllCompleteBroadcasted  = false;

    if (StreamingWave != nullptr)
    {
        StreamingWave->SetStopSoundOnPlaybackFinish(false);
        StreamingWave->ResetStreamingBuffer();
    }
}

void UInoAgentsLiteRtLmDialogueQueue::SetPauseDurationMs(int32 InDurationMs)
{
    DefaultPauseDurationMs = FMath::Max(InDurationMs, 0);
}

// ======================================================================
// Auto-bound conversation handlers
// ======================================================================

void UInoAgentsLiteRtLmDialogueQueue::HandleSentenceFromConversation(
    FString RawText, FString /*CleanText*/)
{
    // Send RawText with [emotion] tags intact — ElevenLabs consumes
    // them as delivery instructions. Strip {curly} emotion state
    // tags which ElevenLabs doesn't understand and would speak aloud.
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

void UInoAgentsLiteRtLmDialogueQueue::HandleNewLineFromConversation()
{
    // Insert a pause slot on newline. Deduplicated: if the last slot
    // is already a pause (e.g. EnqueueSentenceInternal just inserted
    // one before the next sentence), skip.
    if (DefaultPauseDurationMs <= 0)
    {
        return;
    }
    if (Slots.Num() > 0 && Slots.Last().bIsPause)
    {
        return;
    }
    EnqueuePauseInternal();
    DrainReadySlots();
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

    // Insert a silence-pause slot before every sentence except the
    // first. Deduped: if the prior slot is already a pause (from a
    // prior OnNewLine fire), skip.
    if (DefaultPauseDurationMs > 0
        && Slots.Num() > 0
        && !Slots.Last().bIsPause)
    {
        EnqueuePauseInternal();
    }

    // Now add the sentence slot.
    const int32 SlotIndex = Slots.Num();
    Slots.AddDefaulted();
    bAllCompleteBroadcasted = false;

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

    UInoAgentsLiteRtLmDialogueSlotObserver* Observer =
        NewObject<UInoAgentsLiteRtLmDialogueSlotObserver>(this);
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

    // Drain once both the pause slot (if any) and the sentence slot
    // are in place. If the sentence slot is incomplete (typical —
    // TTS just fired), drain will process any ready pause slot and
    // stop at the sentence.
    DrainReadySlots();
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
    bAllCompleteBroadcasted = false;

    UE_LOG(LogInoAgents, Verbose,
           TEXT("UInoAgentsLiteRtLmDialogueQueue: slot %d is a %d-ms silence"),
           Slots.Num() - 1, DefaultPauseDurationMs);
}

// ======================================================================
// Wave feed helpers
// ======================================================================

void UInoAgentsLiteRtLmDialogueQueue::FeedBytesToWave(const TArray<uint8>& Bytes)
{
    if (StreamingWave == nullptr || Bytes.Num() == 0)
    {
        return;
    }

    if (bDerivedFormatIsMP3)
    {
        StreamingWave->AppendAudioDataFromMP3(Bytes);
    }
    else
    {
        StreamingWave->AppendAudioDataFromRAW(
            Bytes,
            EInoAgentsRAWAudioFormat::Int16,
            DerivedSampleRate,
            DerivedNumChannels);
    }
}

void UInoAgentsLiteRtLmDialogueQueue::InjectSilence(int32 PauseMs)
{
    if (StreamingWave == nullptr || PauseMs <= 0 || DerivedSampleRate <= 0)
    {
        return;
    }

    // N frames of silence = rate * ms / 1000. Silence is always
    // Int16 zeros regardless of surrounding MP3/PCM format — the
    // wave transcodes to its internal float32 buffer either way.
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
        Silence,
        EInoAgentsRAWAudioFormat::Int16,
        DerivedSampleRate,
        Channels);

    UE_LOG(LogInoAgents, Verbose,
           TEXT("UInoAgentsLiteRtLmDialogueQueue: injected %d frames of silence (%d ms)"),
           static_cast<int32>(NumFrames), PauseMs);
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

    FSlot& Slot = Slots[SlotIndex];

    if (SlotIndex == CurrentPlayIndex)
    {
        // If anything accumulated while this slot wasn't current yet
        // (race between OnSlotChunk and DrainReadySlots advancing),
        // flush those first so byte order stays correct.
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
    while (Slots.IsValidIndex(CurrentPlayIndex))
    {
        FSlot& Slot = Slots[CurrentPlayIndex];

        // Flush any bytes that accumulated while this slot wasn't
        // current. Runs on every iteration — safe for incomplete
        // slots (they'll get more chunks later, OnSlotChunk will
        // feed those directly since BufferedBytes is empty after
        // this flush). Critical for byte ordering: without this,
        // a chunk that arrived before the slot became current would
        // play AFTER live chunks that arrive once it's current.
        if (!Slot.bIsPause
            && !Slot.bErrored
            && Slot.BufferedBytes.Num() > 0)
        {
            FeedBytesToWave(Slot.BufferedBytes);
            Slot.BufferedBytes.Reset();
        }

        // Can't advance past an incomplete slot. The next
        // OnSlotChunk / OnSlotComplete will re-enter drain.
        if (!Slot.bComplete)
        {
            break;
        }

        // Pause slot: inject silence samples into the wave buffer so
        // the silence plays back perfectly synced with the surrounding
        // audio (no wall-clock timer — that would fire while prior
        // sentence data is still ahead of the playback cursor).
        if (Slot.bIsPause)
        {
            InjectSilence(Slot.PauseDurationMs);
            CurrentPlayIndex++;
            continue;
        }

        // Audio slot: flush already handled above.
        CurrentPlayIndex++;

        UE_LOG(LogInoAgents, Verbose,
               TEXT("UInoAgentsLiteRtLmDialogueQueue: advanced to slot %d"),
               CurrentPlayIndex);
    }

    if (CurrentPlayIndex >= Slots.Num()
        && Slots.Num() > 0
        && !bAllCompleteBroadcasted)
    {
        // All slots drained. Flip the wave into drain mode so it
        // emits OnAudioPlaybackFinished when its buffer empties and
        // the audio component naturally stops.
        if (StreamingWave != nullptr)
        {
            StreamingWave->SetStopSoundOnPlaybackFinish(true);
        }

        bAllCompleteBroadcasted = true;

        UE_LOG(LogInoAgents, Log,
               TEXT("UInoAgentsLiteRtLmDialogueQueue: all %d slot(s) dispatched"),
               Slots.Num());

        OnAllComplete.Broadcast();
    }
}
