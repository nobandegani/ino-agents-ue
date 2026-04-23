// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Interfaces/IHttpRequest.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/UniquePtr.h"

#include "Chatterbox/InoChatterboxTypes.h"

#include "InoChatterboxTtsSubsystem.generated.h"

// Forward declarations — these are private classes under
// Private/Chatterbox/ that we own via TUniquePtr. The subsystem .cpp
// includes their full headers where the complete type is needed.
//
// Because TUniquePtr<IncompleteType>'s deleter instantiates with the
// complete type, the default constructor / destructor / FVTableHelper
// constructor MUST be declared out-of-line (below) and defined in
// InoChatterboxTtsSubsystem.cpp. Leaving any of them implicit produces
// C4150 "delete of pointer to incomplete type" — exact same gotcha as
// UInoLiteRtLmConversation documents for its worker TUniquePtr.
class FInoChatterboxModels;
class FInoChatterboxSynthesisWorker;
class FInoChatterboxTokenizer;

/**
 * One file in the Chatterbox download queue — internal detail of the
 * auto-download flow. Declared at namespace scope (not nested in
 * UInoChatterboxTtsSubsystem) so the build-queue helper in the
 * subsystem's .cpp anonymous namespace can reference it without
 * friending or exposing a private type. Not UPROPERTY/USTRUCT —
 * no Blueprint visibility; pure C++ implementation detail.
 */
struct FInoChatterboxDownloadFile
{
    /** Absolute URL to fetch. */
    FString Url;
    /** Absolute target path on disk (not the .partial). */
    FString TargetPath;
    /** If true, a 404 or error on this file fails the whole load.
     *  False for .onnx_data companions — some variants inline weights
     *  and the server legitimately returns 404 for them. */
    bool    bRequired = true;
    /** Size learned from HEAD probe. -1 = unknown (HF sometimes
     *  strips Content-Length across its CDN redirect; treat as
     *  "report bytes-only, not percent" downstream). */
    int64   ExpectedBytes = -1;
    /** Live byte count during the active GET, latched on completion. */
    int64   BytesWritten = 0;
    /** Set to true once the file is either fully downloaded (success)
     *  or skipped (optional 404). Used to avoid re-downloading. */
    bool    bDone = false;
};

/**
 * Game-instance-wide Chatterbox Turbo TTS runtime owner.
 *
 * ONE instance per game instance (created on game start, destroyed on
 * game shutdown). Accessed via:
 *
 *     UGameInstance* GI = GetGameInstance();
 *     UInoChatterboxTtsSubsystem* Subsys =
 *         GI->GetSubsystem<UInoChatterboxTtsSubsystem>();
 *
 * Owns:
 *   - The loaded FInoChatterboxModels (four ORT sessions — speech_encoder,
 *     embed_tokens, language_model, conditional_decoder)
 *   - The loaded FInoChatterboxTokenizer (GPT-2 BPE + paralinguistic
 *     tag support, parsed from tokenizer.json)
 *   - The currently-loaded variant identity (for
 *     IsModelsLoaded/GetLoadedVariant Blueprint queries)
 *
 * Single-variant invariant:
 *   Only one variant is resident at a time. To switch, call
 *   UnloadModels() then LoadModelsAsync() with a new variant. This
 *   matches UInoLiteRtLmSubsystem's one-engine-at-a-time ergonomics
 *   and bounds the subsystem's peak RAM at ~1.5 GB (fp16) / ~510 MB
 *   (q4f16).
 *
 * Lifecycle:
 *   Initialize()        : UE calls at game start. Zero-inits members.
 *                         Does NOT load a model (a 500+ MB load would
 *                         hitch the editor on Play-In-Editor).
 *   LoadModelsAsync()   : Game code calls to download (if needed) +
 *                         load a variant. Async. Fires OnLoaded on the
 *                         game thread when done.
 *   SynthesizeAsync()   : Per-utterance TTS. Dispatches to a worker
 *                         thread, marshals OnComplete back to the game
 *                         thread with the PCM waveform.
 *   UnloadModels()      : Frees the 4 ORT sessions + tokenizer. Safe
 *                         with nothing loaded.
 *   Deinitialize()      : UE calls at game shutdown. Cancels in-flight
 *                         operations, calls UnloadModels.
 *
 * Threading:
 *   All UFUNCTIONs MUST be called from the game thread. The subsystem
 *   dispatches heavy work (model load, synthesis) to ThreadPool workers
 *   and marshals completion callbacks back via AsyncTask(GameThread).
 *   Delegate handlers fire on the game thread — safe to touch UObjects.
 *
 * NOTE (Commit 1 scaffolding): LoadModelsAsync and SynthesizeAsync are
 * currently stubs that fail immediately with an error message. The real
 * implementations land in Commits 2 (load, files already on disk),
 * 3 (synth + cancel + worker thread), and 4 (download flow).
 * IsModelDownloaded is fully implemented in Commit 1 because it's pure
 * file-IO and useful for Blueprint dev work.
 */
UCLASS()
class INOAGENTS_API UInoChatterboxTtsSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    // Out-of-line ctor/dtor (see the TUniquePtr / incomplete-type
    // comment above the forward declarations). UHT generates TWO
    // implicit constructors for every UCLASS: the default ctor AND a
    // hot-reload vtable helper ctor (DEFINE_VTABLE_PTR_HELPER_CTOR_NS).
    // BOTH must be declared here and supplied out-of-line in the cpp
    // where the forward-declared types are complete, otherwise UHT's
    // generated .gen.cpp emits them inline and fails with C4150.
    // Same gotcha UInoLiteRtLmConversation documents for its worker.
    UInoChatterboxTtsSubsystem();
    UInoChatterboxTtsSubsystem(FVTableHelper& Helper);
    virtual ~UInoChatterboxTtsSubsystem();

    //~ UGameInstanceSubsystem interface
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    //~ End UGameInstanceSubsystem interface

    // ------------------------------------------------------------------
    // Model lifecycle
    // ------------------------------------------------------------------

    /**
     * Asynchronously load a Chatterbox variant (or per-session mix of
     * variants). Returns immediately; the two per-call delegates fire
     * on the game thread:
     *
     *   OnDownloadProgress — fires 0+ times during download only. If
     *     every file is already cached on disk, this never fires.
     *     Payload: (Percent, BytesReceived, TotalBytes, bCompleted).
     *     Percent is aggregate across the whole variant queue.
     *     bCompleted=false on intermediate ticks; bCompleted=true on
     *     ONE terminal tick, fired AFTER every file in the queue has
     *     been atomic-renamed on disk, BEFORE the ThreadPool load
     *     dispatches. Use it to flip UI from "downloading" to
     *     "loading" without waiting for OnLoaded.
     *
     *   OnLoaded — fires exactly ONCE at the end, when the 4 ORT
     *     sessions + tokenizer are ready (bSuccess=true) or a terminal
     *     error prevented load (bSuccess=false, ErrorMessage filled).
     *
     * Flow:
     *   1. Resolve per-session variants from Config.
     *   2. Check per-session files present on disk. If all present,
     *      skip to step 4.
     *   3. Download missing files from HuggingFace. OnDownloadProgress
     *      fires during this phase; bCompleted=true fires at the end
     *      of a successful download.
     *   4. Dispatch ThreadPool: load tokenizer + 4 ORT sessions.
     *   5. Marshal result to the game thread; fire OnLoaded.
     *
     * Error cases that fire OnLoaded with bSuccess=false:
     *   - Another LoadModelsAsync is already in flight
     *   - A variant is already loaded (call UnloadModels first)
     *   - No Project Settings entry for any of the requested variants
     *   - A required file is missing and no URL is configured
     *   - HTTP download failed (network error / 404 / etc.)
     *   - tokenizer.json parse failed
     *   - Any ORT session failed to construct (corrupt / missing
     *     companion / unsupported op / kernel coverage gap)
     *
     * On download failure the OnDownloadProgress.bCompleted=true is
     * NOT fired — the error flows through OnLoaded(false, err).
     *
     * MUST be called on the game thread.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Chatterbox",
              meta = (AutoCreateRefTerm = "OnDownloadProgress,OnLoaded"))
    void LoadModelsAsync(
        const FInoChatterboxModelConfig&        Config,
        const FOnInoChatterboxDownloadProgress& OnDownloadProgress,
        const FOnInoChatterboxModelsLoaded&     OnLoaded);

    /**
     * Destroy the loaded ORT sessions + tokenizer. Safe to call with
     * nothing loaded (no-op). Cancels any in-flight SynthesizeAsync
     * cooperatively before freeing the bundle so the worker can't
     * deref-after-free.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Chatterbox")
    void UnloadModels();

    /**
     * True if LoadModelsAsync has successfully completed and
     * UnloadModels has not yet been called. False during an in-flight
     * load.
     */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Chatterbox")
    bool IsModelsLoaded() const;

    /**
     * Single-variant shorthand: every required file for one variant is
     * present + non-empty on disk. Enough for the non-per-session case
     * where all 4 sessions share the same variant.
     *
     * Required set:
     *   speech_encoder_<v>.onnx
     *   embed_tokens_<v>.onnx
     *   language_model_<v>.onnx
     *   conditional_decoder_<v>.onnx
     *   tokenizer.json
     *
     * .onnx_data companions are not required (some variants inline
     * weights); config.json / generation_config.json aren't either.
     *
     * Cheap file-stat only — safe to poll every frame from UMG.
     */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Chatterbox")
    bool IsModelDownloaded(EInoChatterboxVariant Variant) const;

    /**
     * Per-session check: true iff each of the 4 session files is
     * present under its resolved variant directory AND a tokenizer.json
     * exists under at least ONE of the referenced variant directories.
     *
     * Use this when you've set bUsePerSessionVariants=true — the
     * single-variant IsModelDownloaded() doesn't know about the other
     * three sessions' variants. For the non-per-session case (default)
     * this returns the same result as IsModelDownloaded(Config.Variant).
     *
     * Same cheap-file-stat semantics. Pure, any-thread-safe.
     */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Chatterbox")
    bool IsConfigDownloaded(const FInoChatterboxModelConfig& Config) const;

    /**
     * Returns the variant that's currently loaded, or the default
     * variant (Q4F16) if nothing is loaded. Pair with IsModelsLoaded
     * to distinguish "nothing loaded" from "Q4F16 loaded".
     */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Chatterbox")
    EInoChatterboxVariant GetLoadedVariant() const;

    /**
     * Output sample rate for all Chatterbox Turbo synthesis. ALWAYS
     * returns 24000 — fixed by the conditional_decoder's HiFi-GAN
     * vocoder (trained at 24 kHz) and not configurable at runtime.
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
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "InoAgents|Chatterbox")
    int32 GetOutputSampleRate() const { return 24000; }

    // ------------------------------------------------------------------
    // Synthesis
    // ------------------------------------------------------------------

    /**
     * Synthesize one utterance. Returns immediately; OnComplete fires
     * on the game thread when the waveform is ready (typically 0.5–5 s
     * on desktop CPU, 2–30 s on mobile).
     *
     * Concurrency: multiple SynthesizeAsync calls are queued FIFO on
     * an internal worker thread. Fire-and-forget dialogue playback
     * sentence-by-sentence is the intended use case — the caller does
     * NOT need to await OnComplete before enqueuing the next line.
     *
     * If Voice.WavFilePath AND Voice.ReferenceSamples are both empty,
     * the subsystem falls back to <variant_dir>/default_voice.wav —
     * auto-downloaded alongside the model files by LoadModelsAsync.
     * See FInoChatterboxVoice's header for the full priority list.
     * This means the minimum "load + synth" flow can be a single
     * no-args SynthesizeAsync call.
     *
     * Error cases that fire OnComplete with bSuccess=false:
     *   - No models loaded (call LoadModelsAsync first)
     *   - Voice.WavFilePath cannot be read / is not 24 kHz mono / has
     *     an unsupported WAV format
     *   - Voice empty AND the default voice file was not downloaded
     *     (network 404, opted out, or mid-download interruption)
     *   - Voice.PrecomputedConditioningPath is set (Phase E feature,
     *     not yet implemented — errors with a clear message so
     *     Blueprint graphs wired for Phase E fail loudly today)
     *   - Text is empty
     *   - Synthesis was cancelled via CancelSynthesis or UnloadModels
     *   - Internal runner failure (propagated from
     *     FInoChatterboxRunner::SynthesizeText's OutError)
     *
     * MUST be called on the game thread.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Chatterbox",
              meta = (AutoCreateRefTerm = "OnComplete"))
    void SynthesizeAsync(
        const FString& Text,
        const FInoChatterboxVoice& Voice,
        const FInoChatterboxSynthesisOptions& Options,
        const FOnInoChatterboxSynthesisComplete& OnComplete);

    /**
     * Streaming variant of SynthesizeAsync: dispatches audio chunks
     * on the game thread as the runner produces them, then fires
     * OnComplete once at the end with the full accumulated waveform.
     *
     * Under the hood this shares the same worker thread + FIFO queue
     * as SynthesizeAsync — chunks flow through the same machinery.
     * The difference is StreamChunkTokens > 0 and OnAudioChunk bound:
     * the runner re-runs the conditional_decoder every N generated
     * speech tokens and calls OnAudioChunk with the NEW bytes (delta).
     *
     * The final chunk (bIsFinal=true in the delegate) fires BEFORE
     * OnComplete. Consumers who want the complete waveform at once
     * can ignore OnAudioChunk and just use OnComplete; consumers who
     * want streaming playback bind OnAudioChunk and start feeding the
     * audio system as each chunk arrives, using OnComplete only for
     * end-of-utterance bookkeeping.
     *
     * StreamChunkTokens is a per-call parameter (not in Options) —
     * set it to 20 (~0.6 s of audio per chunk at typical cadence) for
     * quick first-audio, higher for fewer-but-bigger chunks, or 0
     * to fall back to the non-streaming code path (OnAudioChunk is
     * never called; OnComplete still fires). Values above 512 are
     * effectively "decode once at the end" for most utterances and
     * give no streaming benefit.
     *
     * Same error semantics as SynthesizeAsync — see that method's doc
     * for the failure matrix. On failure, OnAudioChunk is NOT fired at
     * all; only OnComplete fires with bSuccess=false + error message.
     *
     * MUST be called on the game thread.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Chatterbox",
              meta = (AutoCreateRefTerm = "OnAudioChunk,OnComplete"))
    void SynthesizeStreamAsync(
        const FString& Text,
        const FInoChatterboxVoice& Voice,
        const FInoChatterboxSynthesisOptions& Options,
        const FOnInoChatterboxAudioChunk& OnAudioChunk,
        const FOnInoChatterboxSynthesisComplete& OnComplete,
        int32 StreamChunkTokens = 20);

    /**
     * Cooperatively cancel any queued / in-flight synthesis. The
     * currently-running AR iteration completes (tens of ms), then the
     * worker unwinds and fires OnComplete(bSuccess=false,
     * ErrorMessage="Cancelled"). Queued synths that haven't started
     * yet are dropped with the same error.
     *
     * Safe to call with nothing pending (no-op). Non-blocking —
     * returns immediately; the cancel is observed asynchronously by
     * the worker.
     *
     * Automatically triggered by UnloadModels and PIE-end.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Chatterbox")
    void CancelSynthesis();

private:
    // ------------------------------------------------------------------
    // Loaded state
    //
    // Destruction order matters — members are destroyed in reverse
    // declaration order, so Worker (declared last) destructs FIRST
    // when the subsystem is destroyed. That's required because Worker
    // borrows references into Models / Tokenizer; it must join its
    // thread before those refs are freed. UnloadModels makes this
    // explicit too (Worker.Reset() before Models.Reset()), but the
    // auto-destruction order is a safety net.
    // ------------------------------------------------------------------

    /** The 4 ORT sessions. Destroys in LIFO order when reset. */
    TUniquePtr<FInoChatterboxModels> Models;

    /** The GPT-2 BPE tokenizer, loaded once from tokenizer.json. Const
     *  after load, thread-safe for concurrent Encode/Decode. */
    TUniquePtr<FInoChatterboxTokenizer> Tokenizer;

    /** Which variant LoadModelsAsync was called with. Only meaningful
     *  when Models.IsValid(). */
    EInoChatterboxVariant LoadedVariant = EInoChatterboxVariant::Q4F16;

    /** Guards against a second LoadModelsAsync starting while one is
     *  already in flight. Set on dispatch, cleared by the game-thread
     *  result hop-back. */
    bool bLoadInFlight = false;

    /** Set by UnloadModels when called during an in-flight load. The
     *  hop-back reads this flag and, if set, drops the freshly-loaded
     *  bundle instead of assigning it to the subsystem, so the caller
     *  who asked to unload actually ends up unloaded. Cleared on every
     *  hop-back completion. Irrelevant if no load is in flight. */
    bool bPendingUnload = false;

    /** The dedicated worker thread that runs FInoChatterboxRunner on
     *  queued synthesis requests. Created right after a successful
     *  LoadModelsAsync, destroyed by UnloadModels BEFORE Models /
     *  Tokenizer are reset (the worker borrows references to both).
     *  Null when no models are loaded. */
    TUniquePtr<FInoChatterboxSynthesisWorker> Worker;

    // ------------------------------------------------------------------
    // Auto-download state
    //
    // Commit 4 lands the download flow: when LoadModelsAsync is called
    // with a variant that isn't staged on disk, the subsystem walks a
    // hardcoded 11-file queue (4 .onnx + 4 .onnx_data + 3 configs),
    // HEAD-probes each to learn Content-Length for aggregate progress
    // reporting, then downloads them single-shot with .partial
    // staging + atomic rename. Once all files are present, the flow
    // chains into the existing ThreadPool load path (DispatchLoadWorker).
    //
    // HTTP operations run on the game thread (UE's HTTP module marshals
    // completions via the task graph). Only one download is ever in
    // flight at a time — sequential by design, simpler to reason about
    // than a parallel pool.
    // ------------------------------------------------------------------

    /** Build, then consume, during one download session. Cleared in
     *  CleanupDownload. Struct type FInoChatterboxDownloadFile is
     *  declared at namespace scope above (not nested here) so the
     *  build-queue helper in the .cpp's anonymous namespace can see
     *  it without an access workaround. */
    TArray<FInoChatterboxDownloadFile> DownloadQueue;

    /** Index into DownloadQueue of the file currently being HEAD-probed
     *  or GET-downloaded. Advances sequentially. */
    int32 DownloadCursor = 0;

    /** True while we're in the sequential HEAD-probe phase (first pass).
     *  False while we're in the GET-download phase (second pass). */
    bool bDownloadProbing = false;

    /** Live HTTP request in flight. Held to keep the request alive long
     *  enough for its callbacks to fire, and so Deinitialize / UnloadModels
     *  can cancel it cleanly. */
    FHttpRequestPtr DownloadRequest;

    /** Open file handle for the current download's .partial file. Closed
     *  in CleanupDownload. */
    IFileHandle* DownloadFileHandle = nullptr;

    /** The config LoadModelsAsync was called with — we remember it so the
     *  post-download hop into DispatchLoadWorker knows the variant. */
    FInoChatterboxModelConfig PendingConfig;

    /** The OnLoaded delegate LoadModelsAsync was called with — we remember
     *  it so FinishDownloadSuccess / FinishDownloadError can fire it. */
    FOnInoChatterboxModelsLoaded PendingOnLoaded;

    /** Per-call download-progress handler, stashed here for the
     *  duration of the load. Every progress tick (including the
     *  bCompleted=true terminal tick inside FinishDownloadSuccess)
     *  fires through this delegate. Reassigned at each LoadModelsAsync
     *  so a prior load's stale delegate can't accidentally receive
     *  events for a new one. */
    FOnInoChatterboxDownloadProgress PendingOnDownloadProgress;

    /** Download-flow helpers — see InoChatterboxTtsSubsystem.cpp for the
     *  narrative; flow is:
     *    StartDownload           (called when files missing)
     *     └─ StartHeadProbe       (sequential HEADs for aggregate total)
     *         └─ HandleHeadComplete / advance cursor
     *             └─ StartNextFileDownload  (sequential GETs)
     *                 └─ HandleDownloadProgress / HandleDownloadComplete
     *                     └─ FinishDownloadSuccess  (chains into DispatchLoadWorker)
     *                     or  FinishDownloadError   (fires PendingOnLoaded false)
     *                     or  CleanupDownload       (state reset, called from both) */
    void StartDownload();
    void StartHeadProbe();
    void HandleHeadComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
    void StartNextFileDownload();
    // Signature matches FHttpRequestProgressDelegate64 in UE 5.7's
    // Interfaces/IHttpRequest.h — uint64 byte counts (the deprecated
    // int32 variant would overflow for files > 2 GB).
    void HandleDownloadProgress(FHttpRequestPtr Request, uint64 BytesSent, uint64 BytesReceived);
    // Called for each response header as it arrives — used to latch
    // Content-Length into the current file's ExpectedBytes when the
    // HEAD probe didn't return one (HF's CDN sometimes strips it on
    // the 302 → cdn-lfs.hf.co redirect).
    void HandleDownloadHeader(FHttpRequestPtr Request, const FString& HeaderName, const FString& HeaderValue);
    void HandleDownloadComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSucceeded);
    void FinishDownloadSuccess();
    void FinishDownloadError(const FString& Err);
    void CleanupDownload();

    /** Broadcast OnDownloadProgress with the current aggregate. Extracted
     *  because several call sites need to update progress (HEAD complete,
     *  GET progress, GET complete). */
    void BroadcastDownloadProgress();

    /** Shared ThreadPool dispatch — files are on disk, now load them.
     *  Called both from LoadModelsAsync's files-present fast path and
     *  from FinishDownloadSuccess after an auto-download.
     *
     *  Takes the full config so the worker can resolve per-session
     *  variants and their respective directories. For the simple (non-
     *  per-session) case, all four sessions resolve to the same variant
     *  + directory. */
    void DispatchLoadWorker(const FInoChatterboxModelConfig& Config);

    /** Shared worker-enqueue path used by both SynthesizeAsync (streaming
     *  off) and SynthesizeStreamAsync (streaming on). Validates inputs,
     *  resolves reference audio (WavFilePath / ReferenceSamples /
     *  default voice), and pushes a fully-populated FPendingSynth into
     *  the worker. Pass Zero/Unbound for StreamChunkTokens+OnAudioChunk
     *  to get the non-streaming path.
     *
     *  On failure, fires OnComplete synchronously with bSuccess=false and
     *  a diagnostic. Must be called on the game thread. */
    void EnqueueSynth(
        const FString& Text,
        const FInoChatterboxVoice& Voice,
        const FInoChatterboxSynthesisOptions& Options,
        int32 StreamChunkTokens,
        const FOnInoChatterboxAudioChunk& OnAudioChunk,
        const FOnInoChatterboxSynthesisComplete& OnComplete);
};
