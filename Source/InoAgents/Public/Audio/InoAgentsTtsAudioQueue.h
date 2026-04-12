// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "Audio/InoAgentsAudioTypes.h"
#include "ElevenLabs/ElevenLabsTypes.h"

#include "InoAgentsTtsAudioQueue.generated.h"

class UInoAgentsStreamingAudioComponent;
class UInoAgentsTtsAudioQueue;

/**
 * Internal per-slot observer that binds to a single ElevenLabs TTS
 * action's delegates and forwards audio bytes / completion / error
 * back to the parent queue with the slot's sequence index. One
 * instance per EnqueueSentence call.
 *
 * Uses UFUNCTION handlers because ElevenLabs' delegates are
 * DECLARE_DYNAMIC_MULTICAST_DELEGATE which only supports AddDynamic.
 *
 * Not intended for direct Blueprint use — create the queue instead.
 */
UCLASS()
class UInoAgentsTtsSlotObserver : public UObject
{
    GENERATED_BODY()

public:
    int32 SlotIndex = 0;
    TWeakObjectPtr<UInoAgentsTtsAudioQueue> QueueWeak;

    UFUNCTION()
    void HandleAudioChunk(const TArray<uint8>& AudioBytes, int64 TotalBytesReceived);

    UFUNCTION()
    void HandleComplete(const TArray<uint8>& FullAudioBytes, EElevenLabsOutputFormat OutputFormat);

    UFUNCTION()
    void HandleError(FString ErrorMessage);
};

/**
 * Ordered TTS audio queue.
 *
 * Fires ALL ElevenLabs TTS requests in parallel for lowest latency,
 * but plays the resulting audio back through a
 * UInoAgentsStreamingAudioComponent in strict sentence order. If
 * sentence 3 finishes before sentence 2, its bytes are buffered until
 * sentence 2 has been fully drained.
 *
 * The "current" slot (the one whose turn it is to play) streams its
 * chunks directly to the audio component as they arrive — no extra
 * buffering latency for the head-of-line sentence. Future slots
 * accumulate their chunks in a private buffer.
 *
 * Typical Blueprint wiring:
 *
 *   BeginPlay:
 *     Queue = Construct Object From Class (UInoAgentsTtsAudioQueue)
 *     Queue.Initialize(WorldContext, AudioComponent, DefaultVoiceId)
 *
 *   OnSentence(Text):
 *     Queue.EnqueueSentence(Text)
 *
 * That's it — the queue dispatches the TTS, collects the bytes,
 * orders them, and feeds the audio component. No manual slot tracking
 * needed.
 *
 * Does NOT own the audio component — the caller must keep both the
 * queue and the component alive for the duration of the playback.
 */
UCLASS(BlueprintType)
class INOAGENTS_API UInoAgentsTtsAudioQueue : public UObject
{
    GENERATED_BODY()

public:
    /**
     * Set up the queue. Must be called before EnqueueSentence.
     *
     * @param WorldContextObject  Any UObject with a World — used to
     *                            resolve the ElevenLabs subsystem and
     *                            to pass to StreamTextToDialogue.
     * @param InAudioComponent    The streaming audio component that
     *                            will play the ordered audio.
     * @param InDefaultVoiceId    ElevenLabs voice ID used for every
     *                            sentence unless overridden per-call.
     * @param InRequestTemplate   ElevenLabs request settings (ModelId,
     *                            OutputFormat, Stability, Seed, etc.)
     *                            applied to every TTS call. The Inputs
     *                            array is ignored — EnqueueSentence
     *                            fills that in per-sentence. Leave
     *                            fields at their defaults to use the
     *                            subsystem's Project Settings values.
     *                            The audio component's feed format and
     *                            PCM sample rate are auto-derived from
     *                            the template's OutputFormat.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio",
              meta = (WorldContext = "WorldContextObject"))
    void Initialize(UObject* WorldContextObject,
                    UInoAgentsStreamingAudioComponent* InAudioComponent,
                    const FString& InDefaultVoiceId,
                    const FElevenLabsDialogueRequest& InRequestTemplate);

    /**
     * Queue a sentence for TTS. The ElevenLabs request is dispatched
     * immediately (in parallel with any other in-flight sentences).
     * Audio bytes are buffered per-slot and fed to the audio component
     * in strict sentence order.
     *
     * @param SentenceText   The text to synthesise.
     * @param VoiceIdOverride  If non-empty, overrides the default
     *                         voice ID for this sentence only.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    void EnqueueSentence(const FString& SentenceText,
                         const FString& VoiceIdOverride);

    /**
     * Drop all queued (not yet started) AND in-flight slots. Calls
     * StopAndReset on the audio component. After Clear() the queue
     * is ready for a new conversation.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    void Clear();

    /** Fires once when every enqueued slot has been played. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoAgentsAudioFinished OnAllComplete;

    // -----------------------------------------------------------------
    // Internal — called by UInoAgentsTtsSlotObserver
    // -----------------------------------------------------------------
    void OnSlotChunk(int32 SlotIndex, const TArray<uint8>& Bytes);
    void OnSlotComplete(int32 SlotIndex);
    void OnSlotError(int32 SlotIndex, const FString& ErrorMessage);

private:
    struct FSlot
    {
        /** Bytes accumulated from OnAudioChunk while this slot is NOT
         *  the current playback head. Empty for the current slot
         *  because those bytes go straight to the audio component. */
        TArray<uint8> BufferedBytes;

        /** True once the TTS action's OnComplete has fired. */
        bool bComplete = false;

        /** True if an error occurred (skipped on drain). */
        bool bErrored = false;
    };

    TWeakObjectPtr<UObject> WorldContextWeak;

    UPROPERTY()
    TObjectPtr<UInoAgentsStreamingAudioComponent> AudioComponent;

    /** Per-slot observers kept alive via UPROPERTY so GC doesn't
     *  collect them while their TTS action is in flight. */
    UPROPERTY()
    TArray<TObjectPtr<UInoAgentsTtsSlotObserver>> Observers;

    FString DefaultVoiceId;

    /** Template request — all settings except Inputs are copied into
     *  every TTS call dispatched by EnqueueSentence. */
    FElevenLabsDialogueRequest RequestTemplate;

    /** Audio format derived from RequestTemplate.OutputFormat at
     *  Initialize time. Used in FeedAudioBytes calls. */
    EInoAgentsAudioFormat DerivedAudioFormat = EInoAgentsAudioFormat::Mp3;

    TArray<FSlot> Slots;

    /** Index of the slot currently feeding the audio component.
     *  Slots with index < CurrentPlayIndex have already been played. */
    int32 CurrentPlayIndex = 0;

    /** True if we've already called FeedAudioBytes for the current slot
     *  (so subsequent chunks for this slot go directly to the component
     *  rather than buffering). */
    bool bCurrentSlotStreaming = false;

    void DrainReadySlots();
};
