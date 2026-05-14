// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include <atomic>

#include "InoDownloader.h"  // FInoCancellationTokenPtr

#include "NeuTTS/InoNeuTTSTypes.h"

#include "InoNeuTTSSubsystem.generated.h"

class FInoNeuTTSRunner;
class UInoNeuTTSVoiceAsset;

/**
 * Game-instance-wide NeuTTS runtime owner.
 *
 * ONE instance per game instance. Accessed via
 *
 *     UInoNeuTTSSubsystem* Subsys =
 *         GetGameInstance()->GetSubsystem<UInoNeuTTSSubsystem>();
 *
 * Owns:
 *   - One TSharedPtr<FInoNeuTTSRunner> (engine + decoder + voice cache).
 *   - The active-voice state (copy of the primed voice's runtime data).
 *   - Cancellation handles for in-flight download / synth.
 *
 * All public methods MUST be called on the game thread. All delegates
 * fire on the game thread (downloader + AsyncTask marshalling).
 *
 * Single-in-flight invariants:
 *   - One load at a time (`bIsLoading`).
 *   - One voice prime at a time (`bIsPrimingVoice`).
 *   - One synth at a time (`bSynthInFlight`). LiteRT-LM enforces a
 *     single-session-per-engine invariant in its own runtime; serializing
 *     synths at the subsystem level keeps that invariant intact.
 *
 * Lifecycle on Deinitialize: cancels in-flight download, signals synth
 * cancel, drops Runner. In-flight workers see WeakThis go null and exit
 * cleanly without dispatching their completion delegates.
 */
UCLASS()
class INONEUTTS_API UInoNeuTTSSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    //~ UGameInstanceSubsystem interface
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    //~ End UGameInstanceSubsystem interface

    // -----------------------------------------------------------------
    // Model lifecycle
    // -----------------------------------------------------------------

    /**
     * Download (if needed) + load both the backbone (.litertlm) and
     * decoder (.tflite). Fires OnLoaded(true, "") on success.
     *
     * Flow:
     *   1. Resolve backbone + decoder registry entries from
     *      UInoNeuTTSSettings (DisplayName OR LocalFileName,
     *      case-insensitive; empty name → first entry).
     *   2. InoNodes batch-downloads both files (HEAD probe → GET →
     *      .partial → atomic rename → streaming SHA-256). OnDownloadProgress
     *      fires on the game thread with FInoDownloadProgress carrying
     *      OverallProgressPercent across the two files.
     *   3. ThreadPool worker calls FInoNeuTTSRunner::Create (engine +
     *      decoder load + optional warmups).
     *   4. Marshal back to game thread; OnLoaded fires once with the
     *      terminal result.
     *
     * Error cases that fire OnLoaded(false, err):
     *   - Another load is in flight.
     *   - A model is already loaded (call UnloadModel first).
     *   - No registry entry matches Config.BackboneModelName / DecoderModelName.
     *   - A file is missing and its registry entry has no DownloadUrl.
     *   - Download failed (network / HTTP / SHA-256 mismatch).
     *   - Engine or decoder construction failed.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS",
              meta = (AutoCreateRefTerm = "OnLoaded,OnDownloadProgress"))
    void LoadModelAsync(
        const FInoNeuTTSConfig& Config,
        const FInoNeuTTSLoadedDelegate& OnLoaded,
        const FInoNeuTTSDownloadProgressDelegate& OnDownloadProgress);

    /**
     * Pre-stage both files to disk WITHOUT loading them. Useful for
     * app-startup downloads or driving a separate download-progress UI
     * before showing a "Load Model" button.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS",
              meta = (AutoCreateRefTerm = "OnComplete,OnDownloadProgress"))
    void DownloadModelAsync(
        const FInoNeuTTSConfig& Config,
        const FInoNeuTTSLoadedDelegate& OnComplete,
        const FInoNeuTTSDownloadProgressDelegate& OnDownloadProgress);

    /** True iff both backbone + decoder files are present non-empty on
     *  disk for the given config. Pure file-stat probe — no SHA check
     *  (SHA happens inside LoadModelAsync). UMG-safe; call from Tick. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|NeuTTS")
    bool IsModelDownloaded(const FInoNeuTTSConfig& Config) const;

    /** Drop the loaded models. Safe with no model loaded. Implicitly
     *  cancels any in-flight synth. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS")
    void UnloadModel();

    UFUNCTION(BlueprintPure, Category = "InoAgents|NeuTTS")
    bool IsModelLoaded() const;

    UFUNCTION(BlueprintPure, Category = "InoAgents|NeuTTS")
    int32 GetSampleRate() const { return 24000; }

    UFUNCTION(BlueprintPure, Category = "InoAgents|NeuTTS")
    int32 GetNumChannels() const { return 1; }

    // -----------------------------------------------------------------
    // Active voice
    // -----------------------------------------------------------------

    /** Prime the runner's voice cache off the game thread (phonemize ref
     *  text via InoSpeakNG when needed, build speech-tokens block).
     *  Stores the asset's runtime data as the active voice for
     *  subsequent SynthesizeAsync calls. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS",
              meta = (AutoCreateRefTerm = "OnReady"))
    void SetActiveVoiceAsync(
        UInoNeuTTSVoiceAsset* VoiceAsset,
        const FInoNeuTTSVoiceReadyDelegate& OnReady);

    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS")
    void ClearActiveVoice();

    UFUNCTION(BlueprintPure, Category = "InoAgents|NeuTTS")
    bool HasActiveVoice() const { return ActiveVoice.bIsValid; }

    UFUNCTION(BlueprintPure, Category = "InoAgents|NeuTTS")
    FString GetActiveVoiceName() const { return ActiveVoiceName; }

    // -----------------------------------------------------------------
    // Synthesis
    // -----------------------------------------------------------------

    /** One-shot synthesis using the currently-set active voice. Fires
     *  OnComplete on the game thread with the full FInoNeuTTSResult
     *  (carrying 24 kHz mono int16 PCM bytes on success, or an
     *  ErrorMessage on failure). */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS",
              meta = (AutoCreateRefTerm = "OnComplete"))
    void SynthesizeAsync(
        const FString& Text,
        const FInoNeuTTSOptions& Options,
        const FInoNeuTTSSynthesisCompleteDelegate& OnComplete);

    /** Streaming variant. In the current MVP this emits the full PCM as
     *  a single `bIsFinal=true` chunk after backbone + decoder finish —
     *  equivalent to SynthesizeAsync with a trailing OnAudioChunk
     *  callback. Vendor's chunked-decode-with-overlap-add streaming
     *  (test_tts.py reference) is deferred to a future iteration. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS",
              meta = (AutoCreateRefTerm = "OnAudioChunk,OnComplete"))
    void SynthesizeStreamAsync(
        const FString& Text,
        const FInoNeuTTSOptions& Options,
        int32 ChunkTokens,
        const FInoNeuTTSAudioChunkDelegate& OnAudioChunk,
        const FInoNeuTTSSynthesisCompleteDelegate& OnComplete);

    /** Cooperative abort. Synth callback fires OnComplete(false,
     *  "Cancelled") shortly after — granularity is currently up to one
     *  full decode (typically a few seconds). Mid-decode cancellation
     *  is on the roadmap. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS")
    void CancelSynthesis();

    UFUNCTION(BlueprintPure, Category = "InoAgents|NeuTTS")
    bool IsSynthInFlight() const { return bSynthInFlight; }

private:
    // The loaded runner. Shared so in-flight workers can hold a copy
    // and the runner outlives a concurrent UnloadModel.
    TSharedPtr<FInoNeuTTSRunner, ESPMode::ThreadSafe> Runner;

    // Cooperative cancel for the current synth. Re-created at the start
    // of each synth so a stale cancel from a previous synth can't fire
    // on a fresh one.
    TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CurrentCancelFlag;

    // Cancel token for the in-flight download. Set when LoadModelAsync
    // / DownloadModelAsync starts a download; cleared on completion;
    // cancelled in Deinitialize so the downloader stops mid-flight.
    FInoCancellationTokenPtr DownloadCancelToken;

    // Snapshot of the active voice's runtime data. Worker uses this
    // (not the asset) so the synth pipeline doesn't depend on a
    // garbage-collectable UObject.
    FInoNeuTTSVoice ActiveVoice;
    FString         ActiveVoiceName;

    // Per-call pending delegates for the in-flight load/download.
    // Stashed on dispatch, cleared on terminal completion so a stale
    // delegate can't be invoked against a fresh load.
    FInoNeuTTSLoadedDelegate           PendingOnLoaded;
    FInoNeuTTSDownloadProgressDelegate PendingOnDownloadProgress;
    bool bPendingIsDownloadOnly = false;

    // Cached config for the warmup flags. Read by the ThreadPool worker
    // immediately after download success.
    FInoNeuTTSConfig LoadedConfig;

    bool bIsLoading       = false;
    bool bIsPrimingVoice  = false;
    bool bSynthInFlight   = false;

    // --- internal helpers ---

    /** Shared body for LoadModelAsync / DownloadModelAsync. */
    void DownloadEntriesInternal(
        const FInoNeuTTSConfig& Config,
        bool bDownloadOnly,
        const FInoNeuTTSLoadedDelegate& OnComplete,
        const FInoNeuTTSDownloadProgressDelegate& OnDownloadProgress);

    /** ThreadPool dispatch for FInoNeuTTSRunner::Create. Game thread. */
    void DispatchRunnerLoad(const FString& BackbonePath, const FString& DecoderPath);

    /** Terminal completion for the in-flight load/download. Clears
     *  pending delegates and fires OnLoaded. Game thread. */
    void FinishLoad(bool bSuccess, const FString& ErrorMessage);
};
