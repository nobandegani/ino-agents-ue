// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "ElevenLabs/InoElevenLabsTypes.h"

#include "InoLiteRtLmDialogueQueue.generated.h"

class UStreamingSoundWave;            // RuntimeAudioImporter plugin
class UInoLiteRtLmDialogueQueue;
class UInoLiteRtLmConversation;

/** Parameterless dynamic-multicast for queue-level lifecycle events. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnInoDialogueQueueEvent);

/**
 * Internal per-slot observer that binds to a single ElevenLabs TTS
 * action's delegates and forwards audio bytes / completion / error
 * back to the parent queue with the slot's sequence index.
 *
 * Not intended for direct Blueprint use.
 */
UCLASS()
class UInoLiteRtLmDialogueSlotObserver : public UObject
{
    GENERATED_BODY()

public:
    int32 SlotIndex = 0;
    TWeakObjectPtr<UInoLiteRtLmDialogueQueue> QueueWeak;

    UFUNCTION()
    void HandleAudioChunk(const TArray<uint8>& AudioBytes, int64 TotalBytesReceived);

    UFUNCTION()
    void HandleComplete(const TArray<uint8>& FullAudioBytes, EInoElevenLabsOutputFormat OutputFormat);

    UFUNCTION()
    void HandleError(FString ErrorMessage);
};

/**
 * Ordered TTS audio queue — fully automatic.
 *
 * Binds to a UInoLiteRtLmConversation's OnSentence delegate and:
 *   - Dispatches each sentence to ElevenLabs TTS (in parallel — they
 *     stream back on their own clocks).
 *   - Feeds the returned bytes into a RuntimeAudioImporter
 *     UStreamingSoundWave in strict SENTENCE order, regardless of
 *     which TTS finishes first.
 *   - Inserts a configurable silence gap between every pair of
 *     consecutive sentences. The gap is implemented as silence
 *     samples appended to the wave's buffer (timer-free pause —
 *     stays in sync with the playback cursor).
 *
 * The queue does NOT own or drive a UAudioComponent — Blueprint or
 * higher-level code is responsible for plugging the wave into an
 * audio component and calling Play/Stop. Once all slots dispatch,
 * the queue flips the wave to drain mode (SetStopSoundOnPlaybackFinish
 * = true) so the wave's OnAudioPlaybackFinished fires naturally when
 * the audio engine pulls the last sample.
 *
 * The streaming sound wave itself is provided by the
 * RuntimeAudioImporter plugin (UStreamingSoundWave), which handles
 * decoding, format detection, threading, and cleanup. We just
 * orchestrate ordering + silence injection on top.
 */
UCLASS(BlueprintType)
class INOAGENTS_API UInoLiteRtLmDialogueQueue : public UObject
{
    GENERATED_BODY()

public:
    /**
     * Set up the queue and bind to the conversation's delegates.
     *
     * @param WorldContextObject       Any UObject with a World.
     * @param InStreamingWave          UStreamingSoundWave (from
     *                                  RuntimeAudioImporter) the queue
     *                                  feeds TTS bytes into. Caller is
     *                                  responsible for plugging it into
     *                                  a UAudioComponent and driving
     *                                  playback.
     * @param InConversation           The conversation to listen to.
     *                                  Queue binds to OnSentence
     *                                  automatically.
     * @param InDefaultVoiceId         ElevenLabs voice ID.
     * @param InRequestTemplate        ElevenLabs settings (ModelId,
     *                                  OutputFormat, Stability, etc.).
     *                                  Inputs array is ignored; the
     *                                  output format drives the feed
     *                                  format (MP3 vs RAW PCM) and
     *                                  the silence-injection rate.
     * @param InDefaultPauseDurationMs Silence in ms between consecutive
     *                                  sentences. 500 = natural
     *                                  conversational pause. 0 = no
     *                                  gap. Tunable at runtime via
     *                                  SetPauseDurationMs.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio",
              meta = (WorldContext = "WorldContextObject"))
    void Initialize(UObject* WorldContextObject,
                    UStreamingSoundWave* InStreamingWave,
                    UInoLiteRtLmConversation* InConversation,
                    const FString& InDefaultVoiceId,
                    const FInoElevenLabsDialogueRequest& InRequestTemplate,
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
     *  Takes effect on the NEXT pause slot inserted. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    void SetPauseDurationMs(int32 InDurationMs);

    /** Current silence duration between sentences. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Audio")
    int32 GetPauseDurationMs() const { return DefaultPauseDurationMs; }

    /** Fires once when every enqueued slot has been dispatched and the
     *  wave has been flipped to drain mode. Audio may still be playing
     *  at this point — bind the wave's OnAudioPlaybackFinished for the
     *  "audio truly ended" signal. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoDialogueQueueEvent OnAllComplete;

    /** Fires when StopAndReset/Clear runs while a response was still
     *  in-flight (slots present + OnAllComplete not yet broadcast).
     *  Blueprint can bind this to hard-stop its UAudioComponent if
     *  instant silence is desired. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoDialogueQueueEvent OnAudioInterrupted;

    // -----------------------------------------------------------------
    // Internal — called by UInoLiteRtLmDialogueSlotObserver
    // -----------------------------------------------------------------
    void OnSlotChunk(int32 SlotIndex, const TArray<uint8>& Bytes);
    void OnSlotComplete(int32 SlotIndex);
    void OnSlotError(int32 SlotIndex, const FString& ErrorMessage);

private:
    // -----------------------------------------------------------------
    // Auto-bound conversation handler
    // -----------------------------------------------------------------

    UFUNCTION()
    void HandleSentenceFromConversation(FString RawText, FString CleanText);

    // -----------------------------------------------------------------
    // Internal sentence / pause dispatch
    // -----------------------------------------------------------------

    void EnqueueSentenceInternal(const FString& SentenceText);
    void EnqueuePauseInternal();

    /** Feed bytes into the streaming wave using the queue's derived
     *  format (MP3 → AppendAudioDataFromEncoded; PCM → RAW Int16). */
    void FeedBytesToWave(const TArray<uint8>& Bytes);

    /** Append a block of Int16 silence frames matching the queue's
     *  derived sample rate. */
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
    TObjectPtr<UStreamingSoundWave> StreamingWave;

    UPROPERTY()
    TObjectPtr<UInoLiteRtLmConversation> BoundConversation;

    UPROPERTY()
    TArray<TObjectPtr<UInoLiteRtLmDialogueSlotObserver>> Observers;

    FString DefaultVoiceId;
    int32   DefaultPauseDurationMs = 500;

    FInoElevenLabsDialogueRequest RequestTemplate;

    /** true = AppendAudioDataFromEncoded(Mp3), false = AppendAudioDataFromRAW(Int16). */
    bool  bDerivedFormatIsMP3 = true;
    int32 DerivedSampleRate   = 0;
    int32 DerivedNumChannels  = 1;

    TArray<FSlot> Slots;
    int32 CurrentPlayIndex = 0;

    /** Latched after OnAllComplete broadcasts so duplicate drain
     *  iterations don't re-fire it. Reset whenever new slots are
     *  enqueued or the queue is cleared. */
    bool bAllCompleteBroadcasted = false;

    void DrainReadySlots();
};
