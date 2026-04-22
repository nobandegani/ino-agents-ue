// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "Interfaces/IHttpRequest.h"    // FHttpRequestPtr / FHttpResponsePtr typedefs

#include "NeuTtsNano/InoNeuTtsNanoTypes.h"

#include "InoNeuTtsNanoSubsystem.generated.h"

class IFileHandle;

/**
 * Game-instance-wide NeuTTS Nano on-device TTS runtime owner.
 *
 * ONE instance per game instance (created on game start, destroyed on
 * game shutdown). Accessed via:
 *
 *     UGameInstance* GI = GetGameInstance();
 *     UInoNeuTtsNanoSubsystem* Subsys =
 *         GI->GetSubsystem<UInoNeuTtsNanoSubsystem>();
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
 *   - FInoNeuTtsNanoVoiceRegistry (default voice) — Milestone 3
 *   - FInoNeuTtsNanoSynthesisWorker thread — Milestone 3
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
class INOAGENTS_API UInoNeuTtsNanoSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
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
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS Nano")
    void LoadModelAsync(
        const FInoNeuTtsNanoModelConfig& Config,
        const FOnInoNeuTtsNanoModelLoaded& OnLoaded);

    /** Tear down any loaded model + abort any in-flight download. Safe
     *  to call whether or not a load is active. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS Nano")
    void UnloadModel();

    /** True when both files are on disk for the given variant. Pure
     *  file-stat probe; safe to call from Tick. Does NOT verify
     *  checksums. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "InoAgents|NeuTTS Nano")
    bool IsModelDownloaded(EInoNeuTtsNanoBackboneVariant Variant) const;

    /** True when a model is currently loaded in memory and ready for
     *  SynthesizeAsync calls (once synthesis lands in Milestone 4). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "InoAgents|NeuTTS Nano")
    bool IsModelLoaded() const { return bModelLoaded; }

    /** Which variant is currently loaded. Only meaningful when
     *  IsModelLoaded() returns true. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "InoAgents|NeuTTS Nano")
    EInoNeuTtsNanoBackboneVariant GetLoadedVariant() const { return LoadedVariant; }

    /** Cooperatively abort an in-flight download. No-op if no download
     *  is running. Fires PendingOnLoaded with bSuccess=false,
     *  ErrorMessage="Cancelled by caller". */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|NeuTTS Nano")
    void CancelDownload();

    /** Fires periodically during LoadModelAsync's download phase and
     *  once more when each file completes. Percent is 0..100
     *  (clamped). BytesReceived is total bytes on disk across all
     *  files in the queue. TotalBytes is -1 when the aggregate total
     *  isn't yet known (HF CDN sometimes strips Content-Length). */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|NeuTTS Nano")
    FOnInoNeuTtsNanoDownloadProgress OnDownloadProgress;

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
    EInoNeuTtsNanoBackboneVariant LoadedVariant = EInoNeuTtsNanoBackboneVariant::Q4;

    /** Snapshot of the config passed to LoadModelAsync, read by the
     *  load worker. */
    FInoNeuTtsNanoModelConfig PendingConfig;

    /** Dynamic delegate to fire exactly once when LoadModelAsync
     *  resolves. Zeroed after firing. */
    FOnInoNeuTtsNanoModelLoaded PendingOnLoaded;

    // ==================================================================
    // Download state (game-thread only — all callbacks route through
    // the HTTP module's game-thread callback path)
    // ==================================================================

    TArray<FInoNeuTtsNanoDownloadFile> DownloadQueue;

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

    /** Milestone 2 stub: just marks bModelLoaded=true and fires
     *  PendingOnLoaded. Milestone 3 replaces this with the real async
     *  llama_model_load_from_file + FInoOnnxSession::Create dispatch. */
    void DispatchLoadWorker();
};
