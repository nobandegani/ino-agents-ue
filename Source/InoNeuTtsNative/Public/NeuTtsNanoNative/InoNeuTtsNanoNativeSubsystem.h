// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "Interfaces/IHttpRequest.h"    // FHttpRequestPtr / FHttpResponsePtr typedefs

#include "NeuTtsNanoNative/InoNeuTtsNanoNativeTypes.h"

#include "InoNeuTtsNanoNativeSubsystem.generated.h"

class IFileHandle;

// Forward-declared private types — actual definitions in
// Private/NeuTtsNanoNative/. The subsystem holds these via TUniquePtr, so
// the special members (default ctor, FVTableHelper ctor, dtor) are
// defined out-of-line in the .cpp where the private headers are fully
// visible. Same trick UInoLiteRtLmConversation and
// UInoChatterboxTtsSubsystem use for their forward-declared members —
// avoids the classic C4150 "cannot delete pointer to incomplete type"
// UHT .gen.cpp compile error.
class FInoNeuTtsNanoNativeRunner;
class FInoNeuTtsNanoNativeSynthesisWorker;
class FInoNeuTtsNanoNativeVoiceRegistry;

/**
 * Game-instance-wide NeuTTS Nano on-device TTS runtime owner.
 *
 * ONE instance per game instance (created on game start, destroyed on
 * game shutdown). Accessed via:
 *
 *     UGameInstance* GI = GetGameInstance();
 *     UInoNeuTtsNanoNativeSubsystem* Subsys =
 *         GI->GetSubsystem<UInoNeuTtsNanoNativeSubsystem>();
 *
 * Milestone 2 scope (this file):
 *   - Subsystem lifecycle (Initialize / Deinitialize)
 *   - LoadModelAsync — download path only. After both required files
 *     land in PersistentDownloadDir, DispatchLoadWorker() currently
 *     just marks bModelLoaded=true and fires OnLoaded. Milestone 3
 *     fills in the actual llama_model_load_from_file +
 *     FInoOnnxSession::Create work.
 *   - IsModelDownloaded / IsModelLoaded / UnloadModel / CancelDownload
 *   - OnDownloadProgress multicast delegate
 *
 * Not wired yet (added in subsequent milestones):
 *   - SynthesizeAsync / CancelSynthesis — Milestone 4
 *   - FInoNeuTtsNanoNativeVoiceRegistry (default voice) — Milestone 3
 *   - FInoNeuTtsNanoNativeSynthesisWorker thread — Milestone 3
 *
 * Download flow overview:
 *
 *   LoadModelAsync
 *     └─ (local files present?) → DispatchLoadWorker  [fast path]
 *     └─ (otherwise) → StartDownload
 *         ├─ BuildDownloadQueue (backbone GGUF + codec ONNX, both required)
 *         ├─ StartHeadProbe          [2 sequential HEADs, populate sizes]
 *         └─ StartNextFileDownload   [2 sequential GETs with
 *                                     .partial + atomic rename]
 *             └─ HandleDownloadComplete → last file?
 *                 └─ FinishDownloadSuccess → DispatchLoadWorker
 *
 * Mirrors UInoChatterboxTtsSubsystem's multi-file pattern, minus the
 * optional-file / 404-tolerance branch (both NeuTTS files are strictly
 * required). The duplication is flagged tech debt for a future
 * shared Private/InoHttpDownload/ helper.
 *
 * Single-model invariant: only one variant is resident at a time. To
 * switch, call UnloadModel() then LoadModelAsync() with the new config.
 */
UCLASS(DisplayName = "NeuTTS Nano Subsystem")
class INONEUTTSNATIVE_API UInoNeuTtsNanoNativeSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    // Out-of-line special members needed because TUniquePtr<Forward>
    // members below would otherwise try to instantiate their default
    // deleter against an incomplete type in the generated .gen.cpp.
    UInoNeuTtsNanoNativeSubsystem();
    UInoNeuTtsNanoNativeSubsystem(FVTableHelper& Helper);
    virtual ~UInoNeuTtsNanoNativeSubsystem();

    //~ UGameInstanceSubsystem
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    //~ End UGameInstanceSubsystem

    // ==================================================================
    // Public Blueprint API
    // ==================================================================

    /**
     * Kick off an async load of a NeuTTS Nano variant. Dispatches
     * OnLoaded exactly once (game thread) with bSuccess reflecting the
     * final outcome.
     *
     * Flow on cold cache: download both files (~978 MB total, ~1-3 min
     * on typical broadband), then dispatch the model loader. OnProgress
     * fires during the download.
     *
     * Flow on warm cache: no download; loader dispatches immediately.
     *
     * Idempotent: calling LoadModelAsync while a load is in flight will
     * log a warning and fire OnLoaded(false, "...") for the second call.
     * Calling it when the same variant is already loaded returns
     * OnLoaded(true) immediately without reloading.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS Nano",
              meta = (AutoCreateRefTerm = "OnDownloadProgress,OnLoaded"))
    void LoadModelAsync(
        const FInoNeuTtsNanoNativeModelConfig&        Config,
        const FOnInoNeuTtsNanoNativeDownloadProgress& OnDownloadProgress,
        const FOnInoNeuTtsNanoNativeModelLoaded&      OnLoaded);

    /** Tear down any loaded model + abort any in-flight download. Safe
     *  to call whether or not a load is active. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS Nano")
    void UnloadModel();

    /** True when both files are on disk for the given variant. Pure
     *  file-stat probe; safe to call from Tick. Does NOT verify
     *  checksums. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "InoAgents|NeuTTS Nano")
    bool IsModelDownloaded(EInoNeuTtsNanoNativeBackboneVariant Variant) const;

    /** True when a model is currently loaded in memory and ready for
     *  SynthesizeAsync calls (once synthesis lands in Milestone 4). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "InoAgents|NeuTTS Nano")
    bool IsModelLoaded() const { return bModelLoaded; }

    /** Which variant is currently loaded. Only meaningful when
     *  IsModelLoaded() returns true. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "InoAgents|NeuTTS Nano")
    EInoNeuTtsNanoNativeBackboneVariant GetLoadedVariant() const { return LoadedVariant; }

    /**
     * Output sample rate for all NeuTTS Nano synthesis. ALWAYS returns
     * 24000 — fixed by the NeuCodec decoder's architecture (FSQ rate
     * 50 Hz × 480 samples/code → 24 kHz) and cannot be configured at
     * runtime.
     *
     * Use this when constructing a USoundWaveProcedural, feeding bytes
     * into UStreamingSoundWave::AppendAudioDataFromRAW, writing a WAV
     * header via UInoAudioFunctionLibrary::SaveInt16PcmAsWav, or
     * anywhere else you'd otherwise hardcode 24000 — routing through
     * this getter keeps the rate authoritative in one place.
     *
     * Blueprint-pure so it's free to call from a widget Tick or a
     * const-qualified getter.
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "InoAgents|NeuTTS Nano")
    int32 GetOutputSampleRate() const { return 24000; }

    /** Names of voices registered with the subsystem. In v1 this
     *  contains "Default" (baked-in from NeuTtsNanoNative/Resources/
     *  default_voice.nvoice.json) or is empty if the JSON failed to
     *  load. A follow-up milestone will scan a user-provided voices/
     *  directory for additional entries. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "InoAgents|NeuTTS Nano")
    TArray<FName> GetAvailableVoiceNames() const;

    /**
     * Kick off an async synthesis. Returns immediately; OnComplete
     * fires exactly once on the game thread when the synthesis
     * resolves (success = full 24 kHz mono int16 PCM LE bytes;
     * failure = bSuccess=false + ErrorMessage).
     *
     * v1 scope:
     *   - PhonemesText MUST be pre-phonemized IPA (caller's responsibility
     *     until v2 adds an ONNX G2P). Empty string fails at validation.
     *   - VoiceName=NAME_None resolves to "Default".
     *   - Multiple concurrent calls queue up on the worker (FIFO).
     *   - Cancellation aborts the currently-synthesising call only;
     *     queued-but-not-started calls are not cancelled.
     *
     * No-op with OnComplete(false, "Model not loaded.") if LoadModelAsync
     * hasn't succeeded yet.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS Nano")
    void SynthesizeAsync(
        const FString& PhonemesText,
        FName VoiceName,
        const FInoNeuTtsNanoNativeSynthesisOptions& Options,
        const FOnInoNeuTtsNanoNativeSynthesisComplete& OnComplete);

    /**
     * Streaming counterpart of SynthesizeAsync. Fires OnAudioChunk
     * repeatedly during synthesis with incremental delta waveforms
     * (24 kHz mono int16 PCM LE bytes), then fires OnComplete exactly
     * once when the utterance finishes with the full concatenated
     * waveform (same contract as SynthesizeAsync).
     *
     * Event ordering on success:
     *   OnAudioChunk(delta1, bIsFinal=false, n_ids1)
     *   OnAudioChunk(delta2, bIsFinal=false, n_ids2)
     *   ...
     *   OnAudioChunk(deltaN, bIsFinal=true,  n_idsN)   ← exactly one
     *   OnComplete(true, full_concatenated, "")
     *
     * Event ordering on failure (any stage): zero or more
     * OnAudioChunk(..., bIsFinal=false, ...) broadcasts, then
     * OnComplete(false, {}, "error..."). The bIsFinal=true broadcast
     * is NOT guaranteed on failure — bind handlers to OnComplete for
     * end-of-stream detection, not to OnAudioChunk's bIsFinal flag.
     *
     * Chunk cadence is controlled by Options.StreamChunkTokens (see
     * FInoNeuTtsNanoNativeSynthesisOptions for the tradeoff discussion).
     * Passing StreamChunkTokens=0 falls back to one-shot semantics —
     * exactly one OnAudioChunk fires with the full waveform and
     * bIsFinal=true immediately before OnComplete.
     *
     * Re-running the NeuCodec decoder on a growing prefix of speech-ids
     * every StreamChunkTokens tokens is extra CPU work (roughly
     * O(N^2 / StreamChunkTokens) vs O(N) for one-shot). The trade is
     * first-audio latency, not throughput — tune StreamChunkTokens
     * against how quickly you want to start hearing audio.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS Nano")
    void SynthesizeStreamAsync(
        const FString& PhonemesText,
        FName VoiceName,
        const FInoNeuTtsNanoNativeSynthesisOptions& Options,
        const FOnInoNeuTtsNanoNativeAudioChunk& OnAudioChunk,
        const FOnInoNeuTtsNanoNativeSynthesisComplete& OnComplete);

    /** Cooperatively abort the currently-synthesising request (if any).
     *  The worker acknowledges at the next cancel-check point inside
     *  its AR loop (every ~256 tokens, so typically sub-second) and
     *  fires OnComplete(false, "Cancelled by caller."). Calls queued
     *  behind the cancelled one continue normally. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS Nano")
    void CancelSynthesis();

    /** Cooperatively abort an in-flight download. No-op if no download
     *  is running. Fires PendingOnLoaded with bSuccess=false,
     *  ErrorMessage="Cancelled by caller". */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS Nano")
    void CancelDownload();

private:
    // ==================================================================
    // Load state
    // ==================================================================

    /** True once DispatchLoadWorker finishes successfully. Cleared by
     *  UnloadModel. */
    bool bModelLoaded = false;

    /** Set when LoadModelAsync is called, cleared when OnLoaded fires.
     *  Guards against concurrent LoadModelAsync calls. */
    bool bLoadInFlight = false;

    /** Variant most recently requested. Set at LoadModelAsync entry;
     *  promoted to "loaded" on DispatchLoadWorker success. */
    EInoNeuTtsNanoNativeBackboneVariant LoadedVariant = EInoNeuTtsNanoNativeBackboneVariant::Q4;

    /** Snapshot of the config passed to LoadModelAsync, read by the
     *  load worker. */
    FInoNeuTtsNanoNativeModelConfig PendingConfig;

    /** Dynamic delegate to fire exactly once when LoadModelAsync
     *  resolves. Zeroed after firing. */
    FOnInoNeuTtsNanoNativeModelLoaded PendingOnLoaded;

    /** Per-call download-progress handler stashed at LoadModelAsync
     *  entry. Every progress tick (including the bCompleted=true
     *  terminal tick inside FinishDownloadSuccess) fires through this
     *  delegate. Reassigned at each LoadModelAsync so stale delegates
     *  from prior loads can't fire against a fresh one. */
    FOnInoNeuTtsNanoNativeDownloadProgress PendingOnDownloadProgress;

    // ==================================================================
    // Download state (game-thread only — all callbacks route through
    // the HTTP module's game-thread callback path)
    // ==================================================================

    TArray<FInoNeuTtsNanoNativeDownloadFile> DownloadQueue;

    /** Index into DownloadQueue of the file currently being probed
     *  (HEAD phase) or downloaded (GET phase). INDEX_NONE between
     *  phases / when no download is active. */
    int32 DownloadCursor = INDEX_NONE;

    /** True during the HEAD-probe phase; false during GET phase. Used
     *  by completion handlers to dispatch to the right next step. */
    bool bDownloadProbing = false;

    /** In-flight HTTP request. One at a time (serialized cursor). */
    FHttpRequestPtr DownloadRequest;

    /** Open write handle for the current .partial file. Only live
     *  during the active GET. */
    IFileHandle* DownloadFileHandle = nullptr;

    // ==================================================================
    // Download flow helpers (all game thread)
    // ==================================================================

    void StartDownload();
    void StartHeadProbe();
    void HandleHeadComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
    void StartNextFileDownload();
    void HandleDownloadProgress(FHttpRequestPtr Request, uint64 BytesSent, uint64 BytesReceived);
    void HandleDownloadHeader(FHttpRequestPtr Request, const FString& HeaderName, const FString& HeaderValue);
    void HandleDownloadComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
    void BroadcastDownloadProgress();
    void FinishDownloadSuccess();
    void FinishDownloadError(const FString& Err);
    void CleanupDownload();

    /** Dispatches the real async model-load work to the ThreadPool:
     *  llama_model_load_from_file + llama_init_from_model +
     *  FInoOnnxSession::Create for the NeuCodec decoder. Runner is
     *  stashed back onto the game thread via AsyncTask; PendingOnLoaded
     *  fires exactly once with the final outcome. */
    void DispatchLoadWorker();

    // ==================================================================
    // Loaded-state owners (forward-declared; TUniquePtr deleters need
    // full type visibility, so the Reset() call sites live in the .cpp).
    // Reset in UnloadModel in reverse dependency order:
    //   Worker (stops thread) → Runner (frees model/context/ORT session).
    // VoiceRegistry is created at Initialize and lives until Deinitialize;
    // it never holds native resources so teardown ordering doesn't matter.
    // ==================================================================

    TUniquePtr<FInoNeuTtsNanoNativeRunner>           Runner;
    TUniquePtr<FInoNeuTtsNanoNativeSynthesisWorker>  Worker;
    TUniquePtr<FInoNeuTtsNanoNativeVoiceRegistry>    VoiceRegistry;
};
