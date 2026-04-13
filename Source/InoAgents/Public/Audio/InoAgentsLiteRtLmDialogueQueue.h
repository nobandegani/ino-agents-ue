// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Containers/Ticker.h"

#include "Audio/InoAgentsAudioTypes.h"
#include "ElevenLabs/ElevenLabsTypes.h"

#include "InoAgentsLiteRtLmDialogueQueue.generated.h"

class UAudioComponent;
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
 * Pass a ULiteRtLmConversation to Initialize and the queue self-wires
 * to the conversation's OnSentence and OnNewLine delegates. Each line
 * the LLM produces is dispatched to ElevenLabs TTS in parallel, and
 * the resulting audio plays back through a streaming sound wave in
 * strict sentence order with configurable pauses between lines.
 *
 * Internally the queue feeds bytes into a UInoAgentsStreamingSoundWave
 * (the caller-provided wave is used directly — the queue does not own
 * it) which in turn is attached to a UAudioComponent (also caller-
 * provided). The queue drives Play()/Stop() on that component to match
 * the flow of queued slots.
 *
 * Blueprint setup (two nodes total):
 *
 *   Queue = Construct Object From Class (UInoAgentsLiteRtLmDialogueQueue)
 *   Queue.Initialize(self, AudioComp, StreamingWave, Conversation,
 *                    VoiceId, RequestTemplate, PauseDurationMs)
 *
 * No manual wiring of OnSentence / OnNewLine / audio feeds needed —
 * the queue handles everything internally once initialized.
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
     * @param InAudioComponent         Plain UAudioComponent used for
     *                                  playback. Queue calls Play()
     *                                  and Stop() on it to bracket
     *                                  slot flow.
     * @param InStreamingWave          Streaming sound wave fed the
     *                                  TTS bytes. Must already be set
     *                                  as the component's sound.
     * @param InConversation           The conversation to listen to.
     *                                  The queue binds to OnSentence
     *                                  and OnNewLine automatically.
     * @param InDefaultVoiceId         ElevenLabs voice ID for all
     *                                  sentences.
     * @param InRequestTemplate        ElevenLabs settings (ModelId,
     *                                  OutputFormat, Stability, etc.).
     *                                  Inputs array is ignored; the
     *                                  output format drives the feed
     *                                  format (Mp3 vs RAW PCM rate).
     * @param InDefaultPauseDurationMs Silence in ms between lines.
     *                                  500 = natural conversational
     *                                  pause. 0 = no pause.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio",
              meta = (WorldContext = "WorldContextObject"))
    void Initialize(UObject* WorldContextObject,
                    UAudioComponent* InAudioComponent,
                    UInoAgentsStreamingSoundWave* InStreamingWave,
                    ULiteRtLmConversation* InConversation,
                    const FString& InDefaultVoiceId,
                    const FElevenLabsDialogueRequest& InRequestTemplate,
                    int32 InDefaultPauseDurationMs);

    /**
     * Drop all slots, unbind from the conversation, and stop audio.
     * After Clear() the queue can be re-initialized with a new
     * conversation.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    void Clear();

    /**
     * Stop audio playback and drop all pending/playing slots, but keep
     * the conversation binding intact. Call this when the user sends a
     * new message while the previous response is still playing — the
     * queue will pick up the new response's OnSentence events
     * automatically.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    void StopAndReset();

    /** Fires once when every enqueued slot has been dispatched to TTS
     *  and the audio stream's drain flag has been set. Audio may still
     *  be playing at this point — bind the wave's
     *  OnAudioPlaybackFinished for the "audio has truly ended" signal. */
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

    /** Call Play() on the audio component the first time any byte is
     *  handed to the wave after a reset. Idempotent. */
    void EnsurePlaybackStarted();

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
    TObjectPtr<UAudioComponent> AudioComponent;

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
     *  raw int16 PCM at DerivedSampleRate. */
    bool  bDerivedFormatIsMP3 = true;
    int32 DerivedSampleRate   = 0;
    int32 DerivedNumChannels  = 1;

    TArray<FSlot> Slots;
    int32 CurrentPlayIndex      = 0;

    /** Handle for the active pause timer. Cancelled on StopAndReset/Clear
     *  to prevent stale callbacks from firing on cleared queue state. */
    FTSTicker::FDelegateHandle PauseTimerHandle;
    bool  bCurrentSlotStreaming  = false;

    /** True while a pause timer is counting down. Prevents re-entrant
     *  DrainReadySlots calls (from concurrent OnSlotComplete) from
     *  advancing past the consumed pause slot, which would cause the
     *  timer callback to double-advance and skip slots. */
    bool  bPauseTimerPending    = false;

    /** Latched after the first Play() so StopAndReset/Clear know
     *  whether a Stop() call is needed and subsequent feeds don't
     *  re-trigger Play(). */
    bool  bPlaybackStarted      = false;

    void DrainReadySlots();
};
