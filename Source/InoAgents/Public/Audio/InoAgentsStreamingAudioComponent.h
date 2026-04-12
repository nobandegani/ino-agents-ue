// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Components/AudioComponent.h"
#include "Templates/PimplPtr.h"

#include "Audio/InoAgentsAudioTypes.h"

#include "InoAgentsStreamingAudioComponent.generated.h"

class UInoAgentsProceduralWave;

// Forward declaration of the MP3 decoder state held via TPimplPtr. The
// full definition lives in Private/Audio/InoAgentsAudioMp3Decoder.h and
// is only visible from the component's .cpp, so minimp3.h never leaks
// into the public header set.
class FInoAgentsMp3DecodeState;

/**
 * UAudioComponent subclass that plays audio bytes fed in at runtime.
 *
 * Inherits every UAudioComponent feature — volume multiplier, pitch
 * multiplier, attenuation settings, spatialization, source effect chain,
 * sound class, concurrency — so drop one onto an actor and every
 * standard audio control is already in the details panel. This subclass
 * only adds byte-feeding on top.
 *
 * Internally owns a USoundWaveProcedural (created in the constructor)
 * and calls SetSound with it, so every Play/Stop/Pause/FadeIn/FadeOut
 * call goes to the right place without re-implementation.
 *
 * Typical flow (chunked — matches ElevenLabs' OnAudioChunk streaming
 * delivery, but there is no actual coupling to ElevenLabs):
 *
 *     Comp->FeedAudioBytes(Chunk1, EInoAgentsAudioFormat::Mp3);
 *     Comp->FeedAudioBytes(Chunk2, EInoAgentsAudioFormat::Mp3);
 *     ...
 *     Comp->FinalizeStream();
 *
 * Typical flow (one-shot):
 *
 *     Comp->PlayAudio(FullBytes, EInoAgentsAudioFormat::Mp3);
 *
 * Threading: all methods must be called on the game thread. The MP3
 * decoder runs synchronously inside FeedAudioBytes; minimp3 decodes
 * well under a millisecond per frame so this is fine for typical TTS
 * use. A future phase can move decoding to a worker thread without
 * changing this API.
 */
UCLASS(ClassGroup = (InoAgents),
       meta = (BlueprintSpawnableComponent, DisplayName = "Streaming Audio"),
       HideCategories = (Sound))
class INOAGENTS_API UInoAgentsStreamingAudioComponent : public UAudioComponent
{
    GENERATED_BODY()

public:
    UInoAgentsStreamingAudioComponent(const FObjectInitializer& ObjectInitializer);

    // -----------------------------------------------------------------
    // Feeding API
    // -----------------------------------------------------------------

    /**
     * Override the expected PCM sample rate / channel count for the NEXT
     * stream. Only meaningful when subsequent FeedAudioBytes calls use
     * EInoAgentsAudioFormat::PcmInt16 / PcmFloat32 — MP3 overrides these from the frame
     * header on the first decoded frame.
     *
     * Must be called BEFORE the first FeedAudioBytes of a stream.
     * Mid-stream changes are ignored because USoundWaveProcedural rejects
     * sample-rate changes after it starts producing audio.
     *
     * Defaults: 44100 Hz mono.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio",
              meta = (ClampMin = "8000", ClampMax = "192000"))
    void SetPcmFormat(int32 SampleRateHz, int32 NumChannels);

    /**
     * Append audio bytes to the in-flight stream. Callable many times.
     *
     * The first call with a non-empty buffer implicitly starts the
     * stream: prior state is reset, the internal USoundWaveProcedural
     * is configured (sample rate / channel count set), UAudioComponent
     * Play is called, and OnReadyToPlay fires as soon as the first
     * bytes are queued (immediately for PCM, after the first frame
     * decodes for MP3).
     *
     * If Format changes between calls within the same stream, the
     * previous stream is Finalized first and a new one starts.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    void FeedAudioBytes(const TArray<uint8>& AudioBytes,
                        EInoAgentsAudioFormat Format);

    /**
     * Mark the current stream as complete. After this call, the
     * component will fire OnFinished when the procedural-wave queue
     * has fully drained. Does NOT stop playback — already-queued audio
     * keeps playing to completion.
     *
     * Idempotent. No-op if no stream is active.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    void FinalizeStream();

    /**
     * One-shot convenience: FeedAudioBytes + FinalizeStream in a single
     * call. Use this when you already have the full audio buffer in
     * hand — it's a tiny wrapper that saves two Blueprint nodes.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    void PlayAudio(const TArray<uint8>& AudioBytes,
                   EInoAgentsAudioFormat Format);

    /**
     * Abort any in-flight stream and return the component to a clean
     * state. Calls UAudioComponent::Stop internally, flushes the
     * procedural-wave queue, and resets the MP3 decoder state.
     *
     * Does NOT fire OnFinished — that delegate is reserved for the
     * "stream ended cleanly after FinalizeStream" path, so downstream
     * code can tell "I aborted this" apart from "it finished naturally".
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Audio")
    void StopAndReset();

    // -----------------------------------------------------------------
    // Events
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------

    /**
     * How many milliseconds of audio to buffer before starting playback.
     * Higher values give the audio engine more cushion against network
     * jitter (fewer gaps) at the cost of higher latency before the user
     * hears anything.
     *
     *   250 ms — good for conversational TTS (default)
     *   500–1000 ms — bullet-proof for unreliable connections
     *   50–100 ms — low-latency, accepts occasional gaps
     *   0 ms — no pre-buffer, play immediately (may glitch on streaming)
     *
     * Applies per-stream — changing it mid-stream has no effect until
     * the next FeedAudioBytes / PlayAudio call.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Audio",
              meta = (ClampMin = "0", ClampMax = "5000"))
    int32 PreBufferMs = 250;

    // -----------------------------------------------------------------
    // Events
    // -----------------------------------------------------------------

    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoAgentsAudioReadyToPlay OnReadyToPlay;

    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoAgentsAudioFinished OnFinished;

    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoAgentsAudioError OnError;

    /** Fires during playback with batches of PCM float samples.
     *  Batch size is controlled by NumVisualizationSamples. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Audio")
    FOnInoAgentsGeneratePCMData OnGeneratePCMData;

    /** How many samples per OnGeneratePCMData broadcast. Higher =
     *  fewer broadcasts, lower = more granular. 0 = disabled.
     *  Changes only take effect on the next stream (not mid-stream). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "InoAgents|Audio",
              meta = (ClampMin = "0", ClampMax = "16384"))
    int32 NumVisualizationSamples = 0;

    //~ UActorComponent interface
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                               FActorComponentTickFunction* ThisTickFunction) override;
    virtual void BeginDestroy() override;
    //~ End UActorComponent interface

private:
    /**
     * The actual procedural wave bound to our UAudioComponent::Sound.
     * Created in the constructor via CreateDefaultSubobject. Recreated
     * in BeginStreamIfNeeded when the sample rate or channel count
     * changes (USoundWaveProcedural does not allow post-init rate
     * changes — we build a new one).
     */
    UPROPERTY(Transient)
    TObjectPtr<UInoAgentsProceduralWave> ProceduralWave;

    /**
     * MP3 decoder state. Allocated lazily on the first MP3 stream
     * (MakePimpl inside BeginStreamIfNeeded). Reset when the stream
     * ends, when the format changes, or when StopAndReset is called.
     */
    TPimplPtr<FInoAgentsMp3DecodeState> Mp3State;

    EInoAgentsAudioFormat CurrentFormat = EInoAgentsAudioFormat::PcmInt16;

    /** PCM-stream format overrides. Ignored for MP3 streams because
     *  MP3 auto-detects. */
    int32 PcmSampleRate  = 44100;
    int32 PcmNumChannels = 1;

    /** Sample rate actually used by the bound USoundWaveProcedural.
     *  Matches PcmSampleRate for PCM streams and the MP3 frame header
     *  for MP3 streams. Compared at BeginStreamIfNeeded to decide
     *  whether to rebuild the procedural wave. */
    int32 ActiveSampleRate  = 0;
    int32 ActiveNumChannels = 0;

    bool bStreamActive     = false;
    bool bStreamFinalized  = false;
    bool bPlaybackStarted  = false;

    /**
     * Pre-buffer target in bytes. Computed from
     * ActiveSampleRate * ActiveNumChannels * 2 (int16) * PreBufferMs / 1000
     * whenever EnsureProceduralWave (re)builds the procedural wave.
     *
     * Play() is not called until the procedural wave's queue has at
     * least this many bytes, so the audio engine always has a cushion
     * to draw from as network chunks arrive unevenly. Prevents the
     * "gap between chunks" symptom that starves USoundWaveProcedural.
     */
    int32 PreBufferTargetBytes = 0;

    /**
     * Trailing bytes from a PCM chunk that didn't align to a whole
     * sample boundary (e.g. 2-byte boundary for int16, 4-byte for
     * float32). Prepended to the next chunk so sample parity stays
     * correct across HTTP chunk boundaries. Without this, an odd-byte
     * chunk would drop its last byte and every subsequent sample
     * would interpret wrong bytes → noise.
     */
    TArray<uint8> PcmPendingBytes;

    // Internal helpers.
    void BeginStreamIfNeeded(EInoAgentsAudioFormat Format);
    void EnsureProceduralWave(int32 SampleRate, int32 NumChannels);
    void QueuePcmInt16(const int16* Samples, int32 NumSamples);
    void DecodeAndQueueMp3(const TArray<uint8>& Mp3Bytes);
    /** Starts playback once enough audio is buffered. Pass bForce=true
     *  to bypass the threshold check (used by FinalizeStream for short
     *  streams that never cross the pre-buffer target). */
    void TryStartPlayback(bool bForce);
    void ResetInternalState();
};
