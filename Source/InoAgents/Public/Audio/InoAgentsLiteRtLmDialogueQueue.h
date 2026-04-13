// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "Audio/InoAgentsAudioTypes.h"
#include "ElevenLabs/ElevenLabsTypes.h"

#include "InoAgentsLiteRtLmDialogueQueue.generated.h"

class UInoAgentsStreamingSoundWave;
class UInoAgentsLiteRtLmDialogueQueue;
class ULiteRtLmConversation;

/**
 * Internal per-slot observer that binds to a single ElevenLabs TTS
 * action's delegates and forwards audio bytes / completion / error
 * back to the parent queue with the slot's sequence index.
 *
 * Not intended for direct Blueprint use.
 */
UCLASS()
class UInoAgentsLiteRtLmDialogueSlotObserver : public UObject
{
    GENERATED_BODY()

public:
    int32 SlotIndex = 0;
    TWeakObjectPtr<UInoAgentsLiteRtLmDialogueQueue> QueueWeak;

    UFUNCTION()
    void HandleAudioChunk(const TArray<uint8>& AudioBytes, int64 TotalBytesReceived);

    UFUNCTION()
    void HandleComplete(const TArray<uint8>& FullAudioBytes, EElevenLabsOutputFormat OutputFormat);

    UFUNCTION()
    void HandleError(FString ErrorMessage);
};

/**
 * Ordered TTS audio queue — fully automatic.
 *
 * Binds to a ULiteRtLmConversation's OnSentence + OnNewLine and:
 *   - Dispatches each sentence to ElevenLabs TTS (in parallel — they
 *     stream back on their own clocks).
 *   - Feeds the returned bytes into a UInoAgentsStreamingSoundWave in
 *     strict SENTENCE order, regardless of which TTS finishes first.
 *   - Inserts a configurable silence gap between every pair of
 *     consecutive sentences. The gap is implemented as silence
 *     samples appended to the wave's buffer, so playback stays in
 *     perfect sync with the timer-free pause (wall-clock pauses
 *     would fire while the prior sentence's audio was still ahead
 *     of the playback cursor).
 *
 * The queue does NOT own or drive a UAudioComponent — Blueprint or
 * higher-level code is responsible for plugging the wave into an
 * audio component and calling Play/Stop. Once all slots dispatch,
 * the queue flips the wave to drain mode (SetStopSoundOnPlaybackFinish
 * = true) so the wave's OnAudioPlaybackFinished fires naturally when
 * the audio engine pulls the last sample.
 *
 * Blueprint setup:
 *
 *   Queue = Construct Object From Class (UInoAgentsLiteRtLmDialogueQueue)
 *   Queue.Initialize(self, StreamingWave, Conversation,
 *                    VoiceId, RequestTemplate, PauseDurationMs)
 */
UCLASS(BlueprintType)
class INOAGENTS_API UInoAgentsLiteRtLmDialogueQueue : public UObject
{
    GENERATED_BODY()

public:
    /**
     * Set up the queue and bind to the conversation's delegates.
     *
     * @param WorldContextObject       Any UObject with a World.
     * @param InStreamingWave          Streaming sound wave fed the
     *                                  TTS bytes. Caller is responsible
     *                                  for plugging it into a
     *                                  UAudioComponent and driving
     *                                  playback.
     * @param InConversation           The conversation to listen to.
     *                                  Queue binds to OnSentence and
     *                                  OnNewLine automatically.
     * @param InDefaultVoiceId         ElevenLabs voice ID.
     * @param InRequestTemplate        ElevenLabs settings (ModelId,
     *                                  OutputFormat, Stability, etc.).
     *                                  Inputs array is ignored; the
     *                                  output format drives the feed
     *                                  format (MP3 vs RAW PCM) and the
     *                                  silence-injection sample rate.
     * @param InDefaultPauseDurationMs Silence in ms between consecutive
     *                                  sentences. 500 = natural
     *                                  conversational pause. 0 = no
     *                                  gap. Can be changed at runtime
     *                                  via SetPauseDurationMs.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio",
              meta = (WorldContext = "WorldContextObject"))
    void Initialize(UObject* WorldContextObject,
                    UInoAgentsStreamingSoundWave* InStreamingWave,
                    ULiteRtLmConversation* InConversation,
                    const FString& InDefaultVoiceId,
                    const FElevenLabsDialogueRequest& InRequestTemplate,
                    int32 InDefaultPauseDurationMs);

    /**
     * Drop all slots, unbind from the conversation, and reset the wave.
     * After Clear() the queue can be re-initialized with a new
     * conversation.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    void Clear();

    /**
     * Drop all pending/playing slots and reset the wave's buffer, but
     * keep the conversation binding intact. Call when the user sends a
     * new message while the previous response is still playing — the
     * queue will pick up the new response's OnSentence events
     * automatically.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    void StopAndReset();

    /** Change the between-sentence silence duration at runtime.
     *  Takes effect on the NEXT pause slot inserted — slots already
     *  queued keep their pre-change duration. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    void SetPauseDurationMs(int32 InDurationMs);

    /** Current silence duration between sentences. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio")
    int32 GetPauseDurationMs() const { return DefaultPauseDurationMs; }

    /** Fires once when every enqueued slot has been dispatched and
     *  the wave has been flipped to drain mode. Audio may still be
     *  playing at this point — bind the wave's OnAudioPlaybackFinished
     *  for the "audio has truly ended" signal. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoAgentsAudioPlaybackFinished OnAllComplete;

    // -----------------------------------------------------------------
    // Internal — called by UInoAgentsLiteRtLmDialogueSlotObserver
    // -----------------------------------------------------------------
    void OnSlotChunk(int32 SlotIndex, const TArray<uint8>& Bytes);
    void OnSlotComplete(int32 SlotIndex);
    void OnSlotError(int32 SlotIndex, const FString& ErrorMessage);

private:
    // -----------------------------------------------------------------
    // Auto-bound conversation handlers
    // -----------------------------------------------------------------

    UFUNCTION()
    void HandleSentenceFromConversation(FString RawText, FString CleanText);

    UFUNCTION()
    void HandleNewLineFromConversation();

    // -----------------------------------------------------------------
    // Internal sentence / pause dispatch
    // -----------------------------------------------------------------

    void EnqueueSentenceInternal(const FString& SentenceText);
    void EnqueuePauseInternal();

    /** Feed bytes into the streaming wave using the queue's derived
     *  format (MP3 vs. raw int16 PCM). Thread-safe via the wave's
     *  internal task pipe. */
    void FeedBytesToWave(const TArray<uint8>& Bytes);

    /** Append a block of silence frames for the current pause slot.
     *  PauseMs * DerivedSampleRate / 1000 frames, Int16 zeros. */
    void InjectSilence(int32 PauseMs);

    // -----------------------------------------------------------------
    // Types + state
    // -----------------------------------------------------------------

    struct FSlot
    {
        TArray<uint8> BufferedBytes;
        bool bComplete      = false;
        bool bErrored       = false;
        bool bIsPause       = false;
        int32 PauseDurationMs = 0;
    };

    TWeakObjectPtr<UObject> WorldContextWeak;

    UPROPERTY()
    TObjectPtr<UInoAgentsStreamingSoundWave> StreamingWave;

    UPROPERTY()
    TObjectPtr<ULiteRtLmConversation> BoundConversation;

    UPROPERTY()
    TArray<TObjectPtr<UInoAgentsLiteRtLmDialogueSlotObserver>> Observers;

    FString DefaultVoiceId;
    int32   DefaultPauseDurationMs = 500;

    FElevenLabsDialogueRequest RequestTemplate;

    /** Derived audio format for feeding the wave: true = MP3, false =
     *  raw int16 PCM. Rate is always populated (for both MP3 and PCM)
     *  so silence injection knows how many samples to write. */
    bool  bDerivedFormatIsMP3 = true;
    int32 DerivedSampleRate   = 0;
    int32 DerivedNumChannels  = 1;

    TArray<FSlot> Slots;
    int32 CurrentPlayIndex = 0;

    /** Latched after OnAllComplete broadcasts so duplicate drain
     *  iterations (from mid-insertion drain calls, trailing OnNewLine
     *  after completion, etc.) don't re-fire it. Reset whenever new
     *  slots are enqueued or the queue is cleared. */
    bool bAllCompleteBroadcasted = false;

    void DrainReadySlots();
};
