// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"

#include "ChatterboxTurboNative/InoChatterboxTurboNativeTypes.h"

#include "InoChatterboxTurboNativeStreamSynthesize.generated.h"

class UInoChatterboxTurboNativeSubsystem;

/**
 * Fired for each incremental audio chunk the runner produces during
 * streaming synthesis. Multicast so the Blueprint exec pin shape works
 * (BlueprintAssignable delegates are always multicast).
 *
 *   AudioChunk       — NEW bytes since the last chunk, int16 PCM LE,
 *                      24 kHz mono. Append straight into the audio
 *                      player's streaming buffer — the runner guarantees
 *                      no repetition of previously-delivered samples.
 *   bIsFinal         — true on exactly one chunk, the last one. After
 *                      that broadcast, no more OnAudioChunk events fire
 *                      and OnComplete is next.
 *   NumGeneratedTokens — running AR-loop token count at the point this
 *                      chunk was decoded. Drives progress UI — the final
 *                      chunk's value is the authoritative "total tokens
 *                      generated this utterance".
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FInoChatterboxTurboNativeStreamSynthesizeOnChunk,
    const TArray<uint8>&, AudioChunk,
    bool,                 bIsFinal,
    int32,                NumGeneratedTokens);

/**
 * Fired exactly once when streaming synthesis finishes. Mirrors
 * FOnInoChatterboxTurboNativeSynthesisComplete one-to-one — same fields, same
 * semantics; re-declared multicast because BlueprintAssignable requires
 * it and the single-cast variant on the subsystem isn't compatible.
 *
 * Result.AudioSamples is the COMPLETE utterance waveform (concatenation
 * of every OnAudioChunk), kept here as a convenience for consumers that
 * want the whole thing at the end (e.g. saving to a WAV file). Consumers
 * doing live playback can ignore Result.AudioSamples and act purely on
 * the chunk stream + the bSuccess flag.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FInoChatterboxTurboNativeStreamSynthesizeOnComplete,
    bool,                          bSuccess,
    FInoChatterboxTurboNativeSynthesisResult, Result,
    FString,                       ErrorMessage);

/**
 * Fired on any failure path: unloaded subsystem, missing voice, bad WAV,
 * synthesis error, explicit Cancel(). Mutually exclusive with OnComplete
 * — a given UInoChatterboxTurboNativeStreamSynthesize fires exactly one of
 * {OnComplete, OnError} to terminate.
 *
 * Message is a human-readable diagnostic suitable for UI. Same wording
 * as the non-streaming SynthesizeAsync's error path.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FInoChatterboxTurboNativeStreamSynthesizeOnError,
    FString, ErrorMessage);

/**
 * Latent async action that calls UInoChatterboxTurboNativeSubsystem's streaming
 * synthesis path and delivers per-chunk audio back to Blueprint as each
 * chunk is produced.
 *
 * Blueprint usage:
 *   Drag out the "Chatterbox Stream Synthesize" node from any Blueprint
 *   with access to a world context object. Wire:
 *       Text              — the utterance to voice
 *       Voice             — the reference voice (WavFilePath or samples,
 *                           or all-empty to use the default)
 *       Options           — MaxNewTokens + RepetitionPenalty
 *       StreamChunkTokens — how often to emit a chunk (default 20;
 *                           lower = lower first-audio latency, higher
 *                           = fewer-but-larger chunks, zero = no
 *                           streaming / single final OnAudioChunk)
 *   Bind the three exec pins:
 *       On Audio Chunk    — fires multiple times; append to player buffer
 *       On Complete       — fires once at end with full waveform
 *       On Error          — fires once on failure (mutually exclusive
 *                           with OnComplete)
 *
 * C++ usage:
 *     UInoChatterboxTurboNativeStreamSynthesize* Action =
 *         UInoChatterboxTurboNativeStreamSynthesize::StreamSynthesize(
 *             this, Text, Voice, Options, 20);
 *     Action->OnAudioChunk.AddDynamic(this, &UMyClass::HandleChunk);
 *     Action->OnComplete  .AddDynamic(this, &UMyClass::HandleComplete);
 *     Action->OnError     .AddDynamic(this, &UMyClass::HandleError);
 *     Action->Activate();
 *
 * Threading: all delegates fire on the game thread — the worker
 * marshals chunks via AsyncTask(GameThread), so handler targets can
 * touch UObject state safely.
 *
 * Lifetime: on Activate() the action RegisterWithGameInstance's so UE's
 * LatentActionManager holds it alive until FinishCleanly runs
 * SetReadyToDestroy(). No AddToRoot needed. If the subsystem is
 * UnloadModels'd or the game instance dies mid-synthesis, the subsystem
 * fires the underlying OnComplete with bSuccess=false ("cancelled" or
 * "worker shutting down") and this action translates that into OnError.
 *
 * Cancel vs. subsystem-wide CancelSynthesis:
 *   Cancel() on this action removes the action's Ready-to-destroy guard
 *   and fires OnError("cancelled"), but the underlying subsystem-level
 *   synth continues to completion (the synthesis worker doesn't know
 *   which async action submitted the item). To abort the actual
 *   synthesis, call UInoChatterboxTurboNativeSubsystem::CancelSynthesis —
 *   which also fires OnError on every in-flight action. The next
 *   revision may extend the subsystem with per-item cancel if a real
 *   use case arises; Cancel() here today is useful mainly for "I
 *   don't need the result anymore" decisions that don't need to
 *   reclaim the worker's CPU.
 */
UCLASS()
class INOCHATTERBOXNATIVE_API UInoChatterboxTurboNativeStreamSynthesize : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    /**
     * Factory / Blueprint entry point. Does NOT dispatch the synth yet
     * — Activate() runs it once the Blueprint's exec pins are wired.
     * From C++, bind the three delegates on the returned object and
     * call Activate() manually.
     *
     * @param WorldContextObject Any UObject with a World — resolves the
     *                           game instance and, via it, the
     *                           UInoChatterboxTurboNativeSubsystem.
     * @param Text               Utterance text. Non-empty.
     * @param Voice              Reference voice. See FInoChatterboxTurboNativeVoice's
     *                           priority list: WavFilePath →
     *                           ReferenceSamples → default voice fallback.
     * @param Options            MaxNewTokens + RepetitionPenalty. Plain
     *                           struct — editable as a Make node in BP.
     * @param StreamChunkTokens  Chunk cadence in generated AR tokens.
     *                           Default 20 (~0.6 s of audio per chunk).
     *                           0 = no streaming (single terminal
     *                           OnAudioChunk fires with the whole
     *                           utterance alongside OnComplete).
     */
    UFUNCTION(BlueprintCallable,
              meta = (BlueprintInternalUseOnly = "true",
                      WorldContext             = "WorldContextObject",
                      DisplayName              = "Chatterbox Stream Synthesize",
                      AdvancedDisplay          = "StreamChunkTokens"),
              Category = "InoAgents|Chatterbox")
    static UInoChatterboxTurboNativeStreamSynthesize* StreamSynthesize(
        UObject*                              WorldContextObject,
        const FString&                        Text,
        const FInoChatterboxTurboNativeVoice&            Voice,
        const FInoChatterboxTurboNativeSynthesisOptions& Options,
        int32                                 StreamChunkTokens = 20);

    /** See the struct-level comment on FInoChatterboxTurboNativeStreamSynthesizeOnChunk. */
    UPROPERTY(BlueprintAssignable)
    FInoChatterboxTurboNativeStreamSynthesizeOnChunk OnAudioChunk;

    /** See the struct-level comment on FInoChatterboxTurboNativeStreamSynthesizeOnComplete. */
    UPROPERTY(BlueprintAssignable)
    FInoChatterboxTurboNativeStreamSynthesizeOnComplete OnComplete;

    /** See the struct-level comment on FInoChatterboxTurboNativeStreamSynthesizeOnError. */
    UPROPERTY(BlueprintAssignable)
    FInoChatterboxTurboNativeStreamSynthesizeOnError OnError;

    /**
     * Fires OnError("cancelled") and tears the action down. Note: this
     * does NOT abort the underlying subsystem-level synthesis — see the
     * class-level comment. Safe to call from any delegate handler. No-op
     * if the action has already finished.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Chatterbox")
    void Cancel();

    //~ UBlueprintAsyncActionBase interface
    virtual void Activate() override;
    //~ End UBlueprintAsyncActionBase interface

private:
    /** Internal handler: bound to the subsystem's OnAudioChunk delegate.
     *  Forwards chunks verbatim to the BlueprintAssignable multicast. */
    UFUNCTION()
    void HandleChunk(const TArray<uint8>& AudioChunk, bool bIsFinal, int32 NumGeneratedTokens);

    /** Internal handler: bound to the subsystem's OnComplete delegate.
     *  Translates bSuccess into OnComplete vs. OnError. Marks the action
     *  ready-to-destroy afterward. */
    UFUNCTION()
    void HandleComplete(bool bSuccess,
                        FInoChatterboxTurboNativeSynthesisResult Result,
                        FString                       ErrorMessage);

    /** Tear down — unregister from the LatentActionManager and mark
     *  ready-to-destroy. Idempotent. */
    void FinishCleanly();

    /** Inputs captured at factory time; used in Activate(). */
    FString                        PendingText;
    FInoChatterboxTurboNativeVoice            PendingVoice;
    FInoChatterboxTurboNativeSynthesisOptions PendingOptions;
    int32                          PendingStreamChunkTokens = 20;

    /** Weak link to the world context so Activate() can find its subsystem. */
    TWeakObjectPtr<UObject> WorldContextObjectWeak;

    /** Weak ref to the subsystem we submitted to — used in FinishCleanly
     *  to avoid dereferencing after GC. We do NOT hold a strong ref; the
     *  subsystem's game-thread delegate fire is authoritative for our
     *  lifetime end, and the subsystem itself is game-instance-scoped. */
    TWeakObjectPtr<UInoChatterboxTurboNativeSubsystem> SubsystemWeak;

    /** Set once any terminal delegate fires so Cancel() / late callbacks
     *  are no-ops. */
    bool bFinished = false;

    /** Monotonic counter of chunks observed, purely for diagnostic
     *  logging (each HandleChunk logs "chunk #N ..." at Verbose). Not
     *  consulted by any control-flow path. */
    int32 ChunkCount = 0;
};
