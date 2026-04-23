// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Chatterbox/InoChatterboxTtsSubsystem.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Paths.h"

#include "InoAgentsLog.h"
#include "InoAgentsSettings.h"
#include "InoChatterboxAudioIO.h"
#include "InoChatterboxModels.h"
#include "InoChatterboxSynthesisWorker.h"
#include "InoChatterboxTokenizer.h"

namespace
{
    /** Default reference-voice filename, relative to the variant dir.
     *  Shared between BuildDownloadQueue (which adds it to the download
     *  queue) and SynthesizeAsync (which falls back to it when the
     *  caller provides no voice). Defined at file-top so both call
     *  sites above and below see it. */
    constexpr const TCHAR* kDefaultVoiceFilename = TEXT("default_voice.wav");
}

// ============================================================================
// Out-of-line special members
//
// Needed because Models / Tokenizer are TUniquePtr<ForwardDeclaredType>
// in the header. The generated UHT .gen.cpp would otherwise emit the
// default ctor / dtor inline and fail to compile (C4150: "cannot delete
// pointer to incomplete type") because it doesn't #include the
// Chatterbox private headers. Defining them here, where the private
// Chatterbox headers are fully visible, resolves the deleter
// instantiation cleanly. Same trick UInoLiteRtLmConversation uses for
// its worker TUniquePtr.
//
// UHT generates TWO implicit constructors for every UCLASS: the default
// ctor AND a hot-reload vtable helper ctor (DEFINE_VTABLE_PTR_HELPER_CTOR_NS).
// BOTH must be supplied out-of-line.
// ============================================================================

UInoChatterboxTtsSubsystem::UInoChatterboxTtsSubsystem() = default;

UInoChatterboxTtsSubsystem::UInoChatterboxTtsSubsystem(FVTableHelper& Helper)
    : Super(Helper)
{
}

UInoChatterboxTtsSubsystem::~UInoChatterboxTtsSubsystem() = default;

// ============================================================================
// Subsystem lifecycle
// ============================================================================

void UInoChatterboxTtsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    // Zero-init only — never load a model synchronously on startup.
    // A cold load is 1–5 seconds of ORT graph optimization and would
    // hitch PIE. LoadModelsAsync does the heavy lifting off-thread.
    Worker.Reset();
    Models.Reset();
    Tokenizer.Reset();
    LoadedVariant  = EInoChatterboxVariant::Q4F16;
    bLoadInFlight  = false;
    bPendingUnload = false;

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoChatterboxTtsSubsystem::Initialize — ready (no model loaded)"));
}

void UInoChatterboxTtsSubsystem::Deinitialize()
{
    // Cancel any in-flight download so the HTTP completion callback
    // doesn't try to write to a file that's about to be gone. The
    // CleanupDownload inside UnloadModels handles the file handle +
    // queue state; this just stops the network traffic.
    if (DownloadRequest.IsValid())
    {
        DownloadRequest->CancelRequest();
        DownloadRequest.Reset();
    }

    // Free the bundle. UnloadModels is safe to call with nothing loaded.
    UnloadModels();
    Super::Deinitialize();
}

// ============================================================================
// Model lifecycle
// ============================================================================

void UInoChatterboxTtsSubsystem::LoadModelsAsync(
    const FInoChatterboxModelConfig&        Config,
    const FOnInoChatterboxDownloadProgress& OnDownloadProgress,
    const FOnInoChatterboxModelsLoaded&     OnLoaded)
{
    check(IsInGameThread());

    // -------- Pre-flight: reject bad callers synchronously --------

    if (bLoadInFlight)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("Chatterbox LoadModelsAsync: a load is already in flight; rejecting"));
        OnLoaded.ExecuteIfBound(
            false,
            TEXT("A Chatterbox load is already in flight"));
        return;
    }

    if (IsModelsLoaded())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("Chatterbox LoadModelsAsync: models already loaded (variant=%s); ")
               TEXT("call UnloadModels first"),
               *ChatterboxVariantToString(LoadedVariant));
        OnLoaded.ExecuteIfBound(
            false,
            TEXT("Chatterbox models are already loaded; call UnloadModels first"));
        return;
    }

    // -------- Resolve per-session variants --------
    //
    // Simple case (bUsePerSessionVariants=false): all four sessions
    // resolve to Config.Variant — identical to the pre-per-session
    // behavior. Per-session case: each session gets its own variant
    // + directory. Both paths flow through the same downstream code.

    EInoChatterboxVariant EncV, EmbedV, LMV, DecV;
    ChatterboxResolveSessionVariants(Config, EncV, EmbedV, LMV, DecV);

    // Diagnostic: warn if the user's per-session combination mixes
    // activation dtypes. ORT does not cast tensors across session
    // boundaries — the synth will fail at first Run with a dtype
    // error. We still proceed to load so the user can see what
    // happens, but log loudly so they know what's up.
    if (Config.bUsePerSessionVariants)
    {
        const bool bAllFp16 = ChatterboxVariantHasFp16Activations(EncV)
                           && ChatterboxVariantHasFp16Activations(EmbedV)
                           && ChatterboxVariantHasFp16Activations(LMV)
                           && ChatterboxVariantHasFp16Activations(DecV);
        const bool bAllFp32 = !ChatterboxVariantHasFp16Activations(EncV)
                           && !ChatterboxVariantHasFp16Activations(EmbedV)
                           && !ChatterboxVariantHasFp16Activations(LMV)
                           && !ChatterboxVariantHasFp16Activations(DecV);
        if (!bAllFp16 && !bAllFp32)
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("Chatterbox LoadModelsAsync: per-session variants mix ")
                   TEXT("activation dtypes (enc=%s embed=%s lm=%s dec=%s). ")
                   TEXT("ORT will not cast tensors across session boundaries — ")
                   TEXT("synth will fail with a dtype mismatch error at first Run. ")
                   TEXT("Stay within one activation dtype group: fp16 (Q4F16, FP16) ")
                   TEXT("or fp32 (Q4, FP32, Quantized)."),
                   *ChatterboxVariantToString(EncV),
                   *ChatterboxVariantToString(EmbedV),
                   *ChatterboxVariantToString(LMV),
                   *ChatterboxVariantToString(DecV));
        }
    }

    // Remember the config + delegates for the download flow's
    // post-download hop into DispatchLoadWorker, and (if we skip
    // download) for symmetry.
    bLoadInFlight             = true;
    PendingConfig             = Config;
    PendingOnLoaded           = OnLoaded;
    PendingOnDownloadProgress = OnDownloadProgress;

    // -------- Files-present fast path --------
    if (IsConfigDownloaded(Config))
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox LoadModelsAsync: files present (enc=%s embed=%s ")
               TEXT("lm=%s dec=%s), skipping download"),
               *ChatterboxVariantToString(EncV),
               *ChatterboxVariantToString(EmbedV),
               *ChatterboxVariantToString(LMV),
               *ChatterboxVariantToString(DecV));
        DispatchLoadWorker(Config);
        return;
    }

    // -------- Download path --------
    // Files are missing. Look up the model entry for its HF repo URL
    // + revision and kick off the sequential HEAD-probe + GET-download
    // state machine. All subsequent state transitions happen via HTTP
    // callbacks on the game thread; LoadModelsAsync returns here.
    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox LoadModelsAsync: files missing — starting download ")
           TEXT("(enc=%s embed=%s lm=%s dec=%s)"),
           *ChatterboxVariantToString(EncV),
           *ChatterboxVariantToString(EmbedV),
           *ChatterboxVariantToString(LMV),
           *ChatterboxVariantToString(DecV));

    StartDownload();
}

void UInoChatterboxTtsSubsystem::DispatchLoadWorker(
    const FInoChatterboxModelConfig& Config)
{
    check(IsInGameThread());
    check(bLoadInFlight);

    // Loading the 4 ORT sessions + parsing tokenizer.json takes 1–5 s
    // on first run (XNNPACK cache generation) and ~1 s on warm runs.
    // We never block the game thread for this.
    //
    // Capture by value (primitives, copies of the path strings, and
    // the delegate struct). WeakThis guards the final hop-back so we
    // no-op cleanly if the subsystem is torn down mid-load.

    TWeakObjectPtr<UInoChatterboxTtsSubsystem> WeakThis(this);
    const FOnInoChatterboxModelsLoaded         OnLoaded   = PendingOnLoaded;
    // Snapshot the performance options so the worker lambda doesn't
    // reach back into PendingConfig (which could be mutated from the
    // game thread while the worker is mid-load).
    const FInoChatterboxPerformanceOptions     Performance = Config.Performance;
    const double                               TStart     = FPlatformTime::Seconds();

    // Resolve per-session variants up front so the worker lambda is
    // self-contained (no dereferencing this / PendingConfig from the
    // worker thread). For the simple (non-per-session) case, all four
    // resolve to Config.Variant.
    EInoChatterboxVariant EncV, EmbedV, LMV, DecV;
    ChatterboxResolveSessionVariants(Config, EncV, EmbedV, LMV, DecV);

    FInoChatterboxModels::FSessionLoadSpec EncSpec   { ChatterboxResolveVariantDir(EncV),   ChatterboxVariantToString(EncV)   };
    FInoChatterboxModels::FSessionLoadSpec EmbedSpec { ChatterboxResolveVariantDir(EmbedV), ChatterboxVariantToString(EmbedV) };
    FInoChatterboxModels::FSessionLoadSpec LMSpec    { ChatterboxResolveVariantDir(LMV),    ChatterboxVariantToString(LMV)    };
    FInoChatterboxModels::FSessionLoadSpec DecSpec   { ChatterboxResolveVariantDir(DecV),   ChatterboxVariantToString(DecV)   };

    // Tokenizer is variant-agnostic — pick the speech_encoder's dir
    // (guaranteed present if IsConfigDownloaded passed) as the source.
    const FString TokenizerPath = FPaths::Combine(EncSpec.Dir, TEXT("tokenizer.json"));

    // "Representative" variant used by GetLoadedVariant() for
    // single-value UI queries. Speech_encoder's variant is the most
    // natural choice — it's the session most associated with the
    // user's "voice pipeline" decision.
    const EInoChatterboxVariant RepresentativeVariant = EncV;

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox DispatchLoadWorker: dispatching (enc=%s embed=%s ")
           TEXT("lm=%s dec=%s)"),
           *EncSpec.Variant, *EmbedSpec.Variant, *LMSpec.Variant, *DecSpec.Variant);

    Async(EAsyncExecution::ThreadPool,
          [EncSpec, EmbedSpec, LMSpec, DecSpec, TokenizerPath,
           RepresentativeVariant,
           WeakThis, OnLoaded, Performance, TStart]()
    {
        // ============== WORKER THREAD ==============
        //
        // Safety rules (same as LiteRT-LM's pattern):
        //   - Do NOT touch WeakThis here (except to forward it). Weak
        //     pointer dereferencing is only valid from the game thread.
        //   - Prefer to keep logs on the hop-back lambda so output
        //     stays serialized in the order a reader expects.
        //
        // Both loads are synchronous and blocking. Order:
        //   1. Tokenizer (cheap — ~50 ms BPE + vocab build)
        //   2. Models (1-5 s — ORT session creation x4)
        // Stop on first failure so we don't waste seconds loading ORT
        // sessions just to drop them if the tokenizer was corrupt.

        FString LocalError;

        TUniquePtr<FInoChatterboxTokenizer> LocalTokenizer =
            FInoChatterboxTokenizer::LoadFromJson(TokenizerPath, &LocalError);

        TUniquePtr<FInoChatterboxModels> LocalModels;
        if (LocalTokenizer.IsValid())
        {
            LocalModels = FInoChatterboxModels::LoadPerSession(
                EncSpec, EmbedSpec, LMSpec, DecSpec,
                &LocalError, Performance);
        }

        const double ElapsedMs = (FPlatformTime::Seconds() - TStart) * 1000.0;

        // ============== HOP BACK TO GAME THREAD ==============
        //
        // Move the two TUniquePtrs across via init-capture + MoveTemp;
        // the lambda is `mutable` so we can then MoveTemp them again
        // into the subsystem's members.
        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, OnLoaded, ElapsedMs,
             Variant = RepresentativeVariant,
             LocalError = MoveTemp(LocalError),
             LocalTokenizer = MoveTemp(LocalTokenizer),
             LocalModels = MoveTemp(LocalModels)]() mutable
        {
            // Subsystem gone (game instance teardown / PIE stop races
            // our load). TUniquePtr destructors run here when the
            // lambda dies — the 4 ORT sessions and tokenizer tables
            // free cleanly. No leak.
            if (!WeakThis.IsValid())
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("Chatterbox DispatchLoadWorker completion: subsystem is gone; ")
                       TEXT("dropping result"));
                return;
            }

            UInoChatterboxTtsSubsystem* Subsys = WeakThis.Get();
            Subsys->bLoadInFlight = false;

            // UnloadModels was called mid-flight. Drop the result (the
            // TUniquePtrs destruct when this lambda ends) so the
            // caller actually ends up in the unloaded state they
            // asked for.
            if (Subsys->bPendingUnload)
            {
                Subsys->bPendingUnload = false;
                UE_LOG(LogInoAgents, Log,
                       TEXT("Chatterbox DispatchLoadWorker: result dropped after %.1f ms ")
                       TEXT("because UnloadModels was called during load"),
                       ElapsedMs);
                OnLoaded.ExecuteIfBound(
                    false,
                    TEXT("UnloadModels was called during load; result dropped"));
                return;
            }

            const bool bSuccess = LocalTokenizer.IsValid() && LocalModels.IsValid();
            if (!bSuccess)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Chatterbox DispatchLoadWorker: FAILED after %.1f ms: %s"),
                       ElapsedMs, *LocalError);
                OnLoaded.ExecuteIfBound(false, LocalError);
                return;
            }

            // Transfer ownership into the subsystem. Any previous
            // values are impossible here (we guarded IsModelsLoaded up
            // front and bLoadInFlight prevented a concurrent load from
            // racing us in).
            Subsys->Tokenizer     = MoveTemp(LocalTokenizer);
            Subsys->Models        = MoveTemp(LocalModels);
            Subsys->LoadedVariant = Variant;

            // Spin up the synthesis worker now that its borrowed refs
            // (Models + Tokenizer) are stable. The worker's destructor
            // runs in UnloadModels BEFORE these refs are reset, so the
            // invariant holds end-to-end.
            Subsys->Worker = MakeUnique<FInoChatterboxSynthesisWorker>(
                *Subsys->Models, *Subsys->Tokenizer);

            UE_LOG(LogInoAgents, Log,
                   TEXT("Chatterbox DispatchLoadWorker: SUCCESS variant=%s in %.1f ms"),
                   *ChatterboxVariantToString(Variant), ElapsedMs);
            OnLoaded.ExecuteIfBound(true, FString());
        });
    });
}

void UInoChatterboxTtsSubsystem::UnloadModels()
{
    check(IsInGameThread());

    // Teardown order matters: destroy the worker BEFORE the Models /
    // Tokenizer it borrows references from. The worker's destructor
    // joins the thread (which may still be running SynthesizeText),
    // so by the time Worker.Reset() returns the worker thread is
    // gone and nobody is holding references into Models/Tokenizer.

    // -------- Cancel in-flight download --------
    //
    // If a download is in progress, we have two sub-cases:
    //   (a) Actively probing/downloading (DownloadRequest valid) — cancel
    //       the HTTP request; its completion callback won't fire (or will
    //       see bSucceeded=false which CleanupDownload handles).
    //   (b) In between HTTP calls (advancing cursor) — nothing to cancel.
    //
    // Either way, we clean up the queue state, delete any dangling
    // .partial, and fire PendingOnLoaded with a cancellation error so
    // the Blueprint caller's loading-screen UI unwinds.
    const bool bWasDownloading = DownloadQueue.Num() > 0;
    if (bWasDownloading)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("UnloadModels called during download — cancelling download ")
               TEXT("at cursor %d of %d"),
               DownloadCursor, DownloadQueue.Num());

        if (DownloadRequest.IsValid())
        {
            DownloadRequest->CancelRequest();
            DownloadRequest.Reset();
        }

        const FOnInoChatterboxModelsLoaded OnLoadedCopy = PendingOnLoaded;
        CleanupDownload();
        bLoadInFlight  = false;
        bPendingUnload = false;
        OnLoadedCopy.ExecuteIfBound(
            false,
            TEXT("Chatterbox download cancelled (UnloadModels)"));
        // NOTE: no early return — continue on to the Models / Tokenizer
        // teardown below, even though neither is loaded during download,
        // so the function's behaviour reads uniformly.
    }
    else if (bLoadInFlight)
    {
        // A post-download ThreadPool load is in flight. We cannot
        // cancel the ThreadPool worker (tokenizer / ORT session
        // creation is synchronous), but we CAN tell its game-thread
        // hop-back to drop the result so the caller actually ends up
        // unloaded after this returns. Without this flag, the hop-back
        // would blindly assign into Models/Tokenizer seconds later,
        // silently undoing the unload.
        UE_LOG(LogInoAgents, Log,
               TEXT("UnloadModels called during in-flight load — the load's ")
               TEXT("result will be dropped when it completes"));
        bPendingUnload = true;
    }
    else if (Worker.IsValid() || Models.IsValid() || Tokenizer.IsValid())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("UInoChatterboxTtsSubsystem::UnloadModels — clearing variant=%s"),
               *ChatterboxVariantToString(LoadedVariant));
    }

    // Worker first: its destructor blocks on thread join + drains the
    // queue firing "shutting down" errors for any pending items. After
    // this line, no worker thread code can touch Models / Tokenizer.
    Worker.Reset();

    // Now safe to free the borrowed references.
    Tokenizer.Reset();
    Models.Reset();
    LoadedVariant = EInoChatterboxVariant::Q4F16;
}

bool UInoChatterboxTtsSubsystem::IsModelsLoaded() const
{
    // Both must be present — a partial load (models loaded but
    // tokenizer failed) would never have gotten past LoadModelsAsync's
    // hop-back. Guarding on both here is defensive.
    return Models.IsValid() && Tokenizer.IsValid();
}

namespace
{
    bool ChatterboxFileOkNonEmpty(const FString& Path)
    {
        IFileManager& Fm = IFileManager::Get();
        if (!Fm.FileExists(*Path))
        {
            return false;
        }
        // FileSize returns -1 on stat failure; zero-length stubs also
        // fail the > 0 check. Cheap enough to do for every required file.
        return Fm.FileSize(*Path) > 0;
    }

    /** Required component filename for one session at one variant.
     *  Uses ChatterboxVariantToFileSuffix so fp32's suffix-less naming
     *  (matching the HF repo) flows through consistently. */
    FString ChatterboxComponentFilename(const TCHAR* Component, EInoChatterboxVariant Variant)
    {
        return FString::Printf(TEXT("%s%s.onnx"),
            Component, *ChatterboxVariantToFileSuffix(Variant));
    }

    /** Companion .onnx_data filename — same suffix rule. */
    FString ChatterboxComponentDataFilename(const TCHAR* Component, EInoChatterboxVariant Variant)
    {
        return FString::Printf(TEXT("%s%s.onnx_data"),
            Component, *ChatterboxVariantToFileSuffix(Variant));
    }
}

bool UInoChatterboxTtsSubsystem::IsModelDownloaded(EInoChatterboxVariant Variant) const
{
    // Required file set for a runnable variant. Matches
    // FInoChatterboxModels::LoadFromDir's file-discovery logic.
    //
    // .onnx_data companions are intentionally NOT in the required set —
    // some variants inline their weights and don't produce a _data
    // sidecar. ORT handles both cases transparently.
    // config.json / generation_config.json are also NOT in the required
    // set — the runtime pipeline doesn't read them; the PS1 downloads
    // them for completeness only.
    const FString Dir = ChatterboxResolveVariantDir(Variant);

    const FString RequiredFiles[] = {
        ChatterboxComponentFilename(TEXT("speech_encoder"),      Variant),
        ChatterboxComponentFilename(TEXT("embed_tokens"),        Variant),
        ChatterboxComponentFilename(TEXT("language_model"),      Variant),
        ChatterboxComponentFilename(TEXT("conditional_decoder"), Variant),
        FString(TEXT("tokenizer.json")),
    };

    for (const FString& Name : RequiredFiles)
    {
        if (!ChatterboxFileOkNonEmpty(FPaths::Combine(Dir, Name)))
        {
            return false;
        }
    }
    return true;
}

bool UInoChatterboxTtsSubsystem::IsConfigDownloaded(
    const FInoChatterboxModelConfig& Config) const
{
    // Resolve per-session variants and check each session's specific
    // file under its own variant dir. Tokenizer is variant-agnostic —
    // it's enough that ANY of the (possibly-same) 4 variant dirs
    // contains a tokenizer.json.
    EInoChatterboxVariant EncV, EmbedV, LMV, DecV;
    ChatterboxResolveSessionVariants(Config, EncV, EmbedV, LMV, DecV);

    struct FCheck
    {
        const TCHAR*          Component;
        EInoChatterboxVariant Variant;
    };
    const FCheck Checks[] = {
        { TEXT("speech_encoder"),      EncV   },
        { TEXT("embed_tokens"),        EmbedV },
        { TEXT("language_model"),      LMV    },
        { TEXT("conditional_decoder"), DecV   },
    };

    for (const FCheck& C : Checks)
    {
        const FString Dir  = ChatterboxResolveVariantDir(C.Variant);
        const FString File = FPaths::Combine(
            Dir, ChatterboxComponentFilename(C.Component, C.Variant));
        if (!ChatterboxFileOkNonEmpty(File))
        {
            return false;
        }
    }

    // tokenizer.json — check all four variant dirs in case of dedup;
    // if any one of them has a valid file, we're good. (They're
    // variant-agnostic text tokenizers.)
    const EInoChatterboxVariant TokDirs[] = { EncV, EmbedV, LMV, DecV };
    for (const EInoChatterboxVariant V : TokDirs)
    {
        const FString Dir = ChatterboxResolveVariantDir(V);
        if (ChatterboxFileOkNonEmpty(
                FPaths::Combine(Dir, TEXT("tokenizer.json"))))
        {
            return true;
        }
    }
    return false;
}

EInoChatterboxVariant UInoChatterboxTtsSubsystem::GetLoadedVariant() const
{
    return LoadedVariant;
}

// ============================================================================
// Synthesis
// ============================================================================

void UInoChatterboxTtsSubsystem::SynthesizeAsync(
    const FString& Text,
    const FInoChatterboxVoice& Voice,
    const FInoChatterboxSynthesisOptions& Options,
    const FOnInoChatterboxSynthesisComplete& OnComplete)
{
    // Non-streaming path — StreamChunkTokens=0 + unbound OnAudioChunk
    // is the "decoder runs once at the end" case inside EnqueueSynth.
    EnqueueSynth(Text, Voice, Options,
                 /*StreamChunkTokens=*/ 0,
                 FOnInoChatterboxAudioChunk(),
                 OnComplete);
}

void UInoChatterboxTtsSubsystem::SynthesizeStreamAsync(
    const FString& Text,
    const FInoChatterboxVoice& Voice,
    const FInoChatterboxSynthesisOptions& Options,
    const FOnInoChatterboxAudioChunk& OnAudioChunk,
    const FOnInoChatterboxSynthesisComplete& OnComplete,
    int32 StreamChunkTokens)
{
    // Streaming path — forward verbatim. StreamChunkTokens is capped
    // to [0, ...] here (FMath::Max below); zero is a legal value that
    // collapses to the single-final-chunk non-streaming path inside
    // EnqueueSynth. If OnAudioChunk isn't bound we also fall back to
    // non-streaming regardless of StreamChunkTokens.
    EnqueueSynth(Text, Voice, Options,
                 FMath::Max(0, StreamChunkTokens),
                 OnAudioChunk, OnComplete);
}

void UInoChatterboxTtsSubsystem::EnqueueSynth(
    const FString& Text,
    const FInoChatterboxVoice& Voice,
    const FInoChatterboxSynthesisOptions& Options,
    int32 StreamChunkTokens,
    const FOnInoChatterboxAudioChunk& OnAudioChunk,
    const FOnInoChatterboxSynthesisComplete& OnComplete)
{
    check(IsInGameThread());

    // Helper: fire failure synchronously. Used for every pre-flight
    // rejection so callers see a deterministic "same frame" failure
    // before any worker-thread work happens. Streaming callers get
    // the same single terminal OnComplete signal; OnAudioChunk is
    // never fired on the failure path.
    auto FailNow = [&OnComplete](const FString& Msg)
    {
        UE_LOG(LogInoAgents, Warning, TEXT("Chatterbox EnqueueSynth: %s"), *Msg);
        FInoChatterboxSynthesisResult Empty;
        OnComplete.ExecuteIfBound(false, Empty, Msg);
    };

    // -------- Pre-flight: validate inputs --------

    if (!IsModelsLoaded() || !Worker.IsValid())
    {
        FailNow(TEXT("No Chatterbox models loaded — call LoadModelsAsync first"));
        return;
    }

    if (Text.IsEmpty())
    {
        FailNow(TEXT("Text is empty"));
        return;
    }

    // Phase E reserved field — refuse loudly so Blueprints wired for
    // Phase E don't silently run without precomputed conditioning.
    if (!Voice.PrecomputedConditioningPath.IsEmpty())
    {
        FailNow(TEXT("Voice.PrecomputedConditioningPath is reserved for Phase E ")
                TEXT("and not yet implemented — leave it empty and supply ")
                TEXT("WavFilePath or ReferenceSamples"));
        return;
    }

    // -------- Resolve reference audio --------
    //
    // Four sources, priority order (matches FInoChatterboxVoice doc):
    //   1. Voice.WavFilePath — read from disk here on the game thread.
    //      Read is ~1–10 ms for a 5–10 s reference clip (UE's file IO,
    //      no decompression beyond the int16/fp32 passthrough). Short
    //      enough that doing it synchronously is preferable to fighting
    //      the async machinery for a one-shot load at enqueue time.
    //   2. Voice.ReferenceSamples — use as-is. Must already be 24 kHz
    //      mono fp32 (we can't cheaply verify the sample rate of raw
    //      samples).
    //   3. Voice.PrecomputedConditioningPath — Phase E, errored above.
    //   4. Default voice fallback — <variant_dir>/default_voice.wav,
    //      auto-downloaded alongside the model files. Kicks in when
    //      the caller passes an empty FInoChatterboxVoice. Makes the
    //      minimum "LoadModelsAsync + SynthesizeAsync" flow a single
    //      no-args call for prototyping / voice-agnostic uses.
    //
    // Strict 24 kHz policy for WavFilePath: reject mismatch with a clear
    // message. Voice cloning quality is extremely sensitive to resample
    // artifacts and we would rather the developer pre-convert offline
    // than silently degrade.
    FString       ResolvedWavPath;
    bool          bUsingDefaultVoice = false;
    TArray<float> ReferenceAudio;

    if (!Voice.WavFilePath.IsEmpty())
    {
        ResolvedWavPath = Voice.WavFilePath;
    }
    else if (Voice.ReferenceSamples.Num() > 0)
    {
        // Decode int16 PCM LE bytes → float32 samples for the speech
        // encoder. Byte count must be a multiple of 2 (one int16
        // sample per 2 bytes). Raw TArray<uint8> carries no sample-
        // rate metadata, so we document the 24 kHz contract in the
        // struct's header and rely on callers honoring it.
        FString PcmError;
        if (!InoChatterbox::Int16PcmBytesToFloat32Mono(
                Voice.ReferenceSamples, ReferenceAudio, &PcmError))
        {
            FailNow(FString::Printf(
                TEXT("Invalid ReferenceSamples bytes: %s"), *PcmError));
            return;
        }
    }
    else
    {
        // Fall back to the auto-downloaded default voice in the current
        // variant directory. The file was queued with bRequired=false
        // in BuildDownloadQueue, so if the HF repo 404'd or the user
        // killed the download partway through, it may still be missing;
        // in that case we error with a clear message telling them how
        // to unblock (run setup-chatterbox.ps1 -IncludeDefaultVoice, or
        // pass an explicit voice).
        ResolvedWavPath = FPaths::Combine(
            ChatterboxResolveVariantDir(LoadedVariant),
            kDefaultVoiceFilename);
        bUsingDefaultVoice = true;
    }

    if (!ResolvedWavPath.IsEmpty())
    {
        if (bUsingDefaultVoice && !IFileManager::Get().FileExists(*ResolvedWavPath))
        {
            FailNow(FString::Printf(
                TEXT("No voice provided and default voice not found at %s. ")
                TEXT("The default voice is auto-downloaded with LoadModelsAsync ")
                TEXT("but is marked non-required (some deployments opt out), so ")
                TEXT("you may need to either pass an explicit FInoChatterboxVoice ")
                TEXT("with WavFilePath/ReferenceSamples, or run ")
                TEXT("Plugins/InoAgents/Chatterbox/scripts/setup-chatterbox.ps1 ")
                TEXT("-IncludeDefaultVoice to stage it manually."),
                *ResolvedWavPath));
            return;
        }

        int32 WavSampleRate = 0;
        FString WavError;
        if (!InoChatterbox::ReadMonoWavAsFloat32(
                ResolvedWavPath, ReferenceAudio, WavSampleRate, &WavError))
        {
            FailNow(FString::Printf(
                TEXT("Failed to read reference WAV '%s': %s"),
                *ResolvedWavPath, *WavError));
            return;
        }
        if (WavSampleRate != InoChatterbox::kSampleRate)
        {
            FailNow(FString::Printf(
                TEXT("Reference WAV '%s' is %d Hz; Chatterbox Turbo requires 24000 Hz. ")
                TEXT("Pre-convert your clip (e.g. Audacity → Tracks → Resample → 24000) ")
                TEXT("rather than relying on silent in-engine resampling (voice cloning ")
                TEXT("quality is extremely sensitive to resample artifacts)."),
                *ResolvedWavPath, WavSampleRate));
            return;
        }

        if (bUsingDefaultVoice)
        {
            UE_LOG(LogInoAgents, Verbose,
                   TEXT("Chatterbox SynthesizeAsync: no voice supplied — using default voice at %s"),
                   *ResolvedWavPath);
        }
    }

    if (ReferenceAudio.Num() == 0)
    {
        FailNow(TEXT("Resolved reference audio is empty"));
        return;
    }

    // -------- Enqueue --------
    //
    // FIFO across all SynthesizeAsync + SynthesizeStreamAsync calls
    // (same worker, same queue). A caller firing "sentence 1",
    // "sentence 2", "sentence 3" in the same frame gets them played
    // back in order without building its own queue on top — a mix of
    // streaming and non-streaming requests serialises too.

    FInoChatterboxSynthesisWorker::FPendingSynth Item;
    Item.Text              = Text;
    Item.ReferenceAudio    = MoveTemp(ReferenceAudio);
    Item.Options           = Options;
    Item.OnComplete        = OnComplete;
    Item.StreamChunkTokens = StreamChunkTokens;
    Item.OnAudioChunk      = OnAudioChunk;

    const bool bStreaming = OnAudioChunk.IsBound() && StreamChunkTokens > 0;
    Worker->Enqueue(MoveTemp(Item));

    UE_LOG(LogInoAgents, Verbose,
           TEXT("Chatterbox EnqueueSynth: queued (text_len=%d, max_new_tokens=%d, ")
           TEXT("streaming=%s, chunk_tokens=%d)"),
           Text.Len(), Options.MaxNewTokens,
           bStreaming ? TEXT("on") : TEXT("off"),
           StreamChunkTokens);
}

void UInoChatterboxTtsSubsystem::CancelSynthesis()
{
    check(IsInGameThread());

    if (Worker.IsValid())
    {
        UE_LOG(LogInoAgents, Log, TEXT("Chatterbox CancelSynthesis"));
        Worker->CancelAndFlush();
    }
    // No worker = nothing to cancel (either never loaded or already
    // unloaded). Silent no-op — matches the doc on the header.
}

// ============================================================================
// Auto-download flow
//
// High-level flow:
//
//   LoadModelsAsync (files missing)
//     └─ StartDownload
//         ├─ build DownloadQueue from the settings entry
//         └─ StartHeadProbe              [sequential HEADs]
//             └─ HandleHeadComplete → advance DownloadCursor
//                 └─ when cursor hits end, switch to download phase:
//                     └─ StartNextFileDownload  [sequential GETs]
//                         └─ HandleDownloadComplete → write file,
//                             rename .partial → final, advance cursor
//                             └─ when cursor hits end:
//                                 FinishDownloadSuccess → DispatchLoadWorker
//
// The HEAD phase exists so OnDownloadProgress can report aggregate
// Percent across the whole variant (11 files), not just per-file. If
// any HEAD fails to produce Content-Length (HF sometimes strips it
// across the CDN redirect), DownloadAggregateTotal stays -1 and we
// report BytesReceived-only.
// ============================================================================

namespace
{
    /** Derive the HuggingFace base URL (minus trailing slash) + revision
     *  path fragment for one settings entry. */
    void BuildRepoBaseUrl(
        const FInoChatterboxModelEntry& Entry,
        FString&                        OutBase)
    {
        FString RepoUrl = Entry.HuggingFaceRepoUrl;
        while (RepoUrl.EndsWith(TEXT("/"))) { RepoUrl.LeftChopInline(1); }
        const FString Rev = Entry.Revision.IsEmpty() ? TEXT("main") : Entry.Revision;
        OutBase = FString::Printf(TEXT("%s/resolve/%s"), *RepoUrl, *Rev);
    }

    /** Append per-session ONNX + .onnx_data entries to Queue for one
     *  (component, variant) pair. TargetDir is the variant's own
     *  resolved directory (e.g. .../Chatterbox/q4/). Skips if the
     *  target file is already present + non-empty on disk so we don't
     *  re-download cached files. */
    void AppendSessionFiles(
        TArray<FInoChatterboxDownloadFile>& Queue,
        const FString&                      Base,
        const TCHAR*                        Component,
        EInoChatterboxVariant               Variant,
        const FString&                      TargetDir)
    {
        using FFile = FInoChatterboxDownloadFile;

        // Suffix is empty for fp32 (HF names it "speech_encoder.onnx")
        // and "_" + variant for every other variant
        // ("speech_encoder_q4f16.onnx" etc.). URL + local path MUST
        // match so ORT's external-data lookup for the .onnx_data
        // companion resolves — the .onnx file embeds the companion
        // path as a relative filename.
        const FString OnnxName = ChatterboxComponentFilename(Component, Variant);
        const FString DataName = ChatterboxComponentDataFilename(Component, Variant);
        const FString OnnxPath = FPaths::Combine(TargetDir, OnnxName);
        const FString DataPath = FPaths::Combine(TargetDir, DataName);

        if (!ChatterboxFileOkNonEmpty(OnnxPath))
        {
            Queue.Add(FFile{
                /*Url*/        FString::Printf(TEXT("%s/onnx/%s"), *Base, *OnnxName),
                /*TargetPath*/ OnnxPath,
                /*bRequired*/  true,
            });
        }
        // .onnx_data is optional — 404s are tolerated via bRequired=false.
        // We still queue it unless it's already on disk; variants with
        // >2 GB weights need it, tiny variants inline the weights and
        // legitimately 404.
        if (!ChatterboxFileOkNonEmpty(DataPath))
        {
            Queue.Add(FFile{
                /*Url*/        FString::Printf(TEXT("%s/onnx/%s"), *Base, *DataName),
                /*TargetPath*/ DataPath,
                /*bRequired*/  false,
            });
        }
    }

    /** Append the variant-agnostic files (tokenizer.json + configs +
     *  default voice) to Queue under TargetDir, skipping any already
     *  downloaded. These are the same across every variant so we only
     *  need them in ONE variant dir. */
    void AppendSharedFiles(
        TArray<FInoChatterboxDownloadFile>& Queue,
        const FString&                      Base,
        const FString&                      TargetDir)
    {
        using FFile = FInoChatterboxDownloadFile;

        static const TCHAR* const RequiredConfigs[] = {
            TEXT("tokenizer.json"),
            TEXT("config.json"),
            TEXT("generation_config.json"),
        };
        for (const TCHAR* Cfg : RequiredConfigs)
        {
            const FString TargetPath = FPaths::Combine(TargetDir, Cfg);
            if (!ChatterboxFileOkNonEmpty(TargetPath))
            {
                Queue.Add(FFile{
                    /*Url*/        FString::Printf(TEXT("%s/%s"), *Base, Cfg),
                    /*TargetPath*/ TargetPath,
                    /*bRequired*/  true,
                });
            }
        }

        // Default reference voice. Not required — callers supplying
        // their own WavFilePath / ReferenceSamples never touch it, so
        // a 404 or skipped download is fine.
        const FString VoicePath = FPaths::Combine(TargetDir, TEXT("default_voice.wav"));
        if (!ChatterboxFileOkNonEmpty(VoicePath))
        {
            Queue.Add(FFile{
                /*Url*/        TEXT("https://huggingface.co/onnx-community/chatterbox-ONNX/resolve/main/default_voice.wav"),
                /*TargetPath*/ VoicePath,
                /*bRequired*/  false,
            });
        }
    }

    /** Build a download queue that covers every file needed by the
     *  config's per-session variants.
     *
     *  Simple case (bUsePerSessionVariants=false): all 4 sessions
     *  resolve to the same variant, so we queue only that variant's
     *  files in one directory.
     *
     *  Per-session case: for each UNIQUE variant used across the 4
     *  sessions, queue only the session files that use that variant.
     *  Variant-agnostic files (tokenizer, configs, voice) land in ONE
     *  dir — the speech_encoder's — since they're identical across
     *  variants. */
    TArray<FInoChatterboxDownloadFile> BuildDownloadQueue(
        const UInoAgentsSettings*        Settings,
        const FInoChatterboxModelConfig& Config)
    {
        TArray<FInoChatterboxDownloadFile> Queue;
        if (Settings == nullptr)
        {
            return Queue;
        }

        EInoChatterboxVariant EncV, EmbedV, LMV, DecV;
        ChatterboxResolveSessionVariants(Config, EncV, EmbedV, LMV, DecV);

        struct FSessionSpec
        {
            const TCHAR*          Component;
            EInoChatterboxVariant Variant;
        };
        const FSessionSpec Sessions[] = {
            { TEXT("speech_encoder"),      EncV   },
            { TEXT("embed_tokens"),        EmbedV },
            { TEXT("language_model"),      LMV    },
            { TEXT("conditional_decoder"), DecV   },
        };

        for (const FSessionSpec& S : Sessions)
        {
            const FInoChatterboxModelEntry* Entry =
                Settings->FindChatterboxModel(S.Variant);
            if (Entry == nullptr)
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("Chatterbox BuildDownloadQueue: no settings entry for ")
                       TEXT("%s's variant '%s' — skipping. LoadModelsAsync will ")
                       TEXT("fail with a clearer error at session load."),
                       S.Component,
                       *ChatterboxVariantToString(S.Variant));
                continue;
            }

            FString Base;
            BuildRepoBaseUrl(*Entry, Base);
            const FString TargetDir = ChatterboxResolveVariantDir(S.Variant);

            IFileManager::Get().MakeDirectory(*TargetDir, /*Tree=*/ true);

            AppendSessionFiles(Queue, Base, S.Component, S.Variant, TargetDir);
        }

        // Shared files go in the speech_encoder's dir. If that dir is
        // missing a settings entry, fall back to the first session that
        // has one.
        const FInoChatterboxModelEntry* SharedEntry =
            Settings->FindChatterboxModel(EncV);
        EInoChatterboxVariant SharedVariant = EncV;
        if (SharedEntry == nullptr)
        {
            for (const FSessionSpec& S : Sessions)
            {
                if (const FInoChatterboxModelEntry* E =
                        Settings->FindChatterboxModel(S.Variant))
                {
                    SharedEntry   = E;
                    SharedVariant = S.Variant;
                    break;
                }
            }
        }
        if (SharedEntry != nullptr)
        {
            FString SharedBase;
            BuildRepoBaseUrl(*SharedEntry, SharedBase);
            const FString SharedDir = ChatterboxResolveVariantDir(SharedVariant);
            IFileManager::Get().MakeDirectory(*SharedDir, /*Tree=*/ true);
            AppendSharedFiles(Queue, SharedBase, SharedDir);
        }

        return Queue;
    }
}   // anonymous namespace

void UInoChatterboxTtsSubsystem::StartDownload()
{
    check(IsInGameThread());
    check(bLoadInFlight);

    // BuildDownloadQueue handles both simple (Config.Variant for all 4
    // sessions) and per-session cases. It looks up the settings entry
    // per variant, builds per-session + shared file URLs, and skips
    // files already cached on disk.
    const UInoAgentsSettings* Settings = UInoAgentsSettings::Get();
    if (Settings == nullptr)
    {
        FinishDownloadError(
            TEXT("Chatterbox download: UInoAgentsSettings unavailable."));
        return;
    }

    // Pre-flight: ensure we have a settings entry for at least one of
    // the variants we're about to load. Without any entry we have no
    // HF repo URL to fetch from and the queue will be empty.
    EInoChatterboxVariant EncV, EmbedV, LMV, DecV;
    ChatterboxResolveSessionVariants(PendingConfig, EncV, EmbedV, LMV, DecV);
    const EInoChatterboxVariant ProbeVariants[] = { EncV, EmbedV, LMV, DecV };
    bool bAnyEntry = false;
    for (const EInoChatterboxVariant V : ProbeVariants)
    {
        if (Settings->FindChatterboxModel(V) != nullptr)
        {
            bAnyEntry = true;
            break;
        }
    }
    if (!bAnyEntry)
    {
        FinishDownloadError(FString::Printf(
            TEXT("Chatterbox download: no Project Settings entries for any of the ")
            TEXT("requested variants (enc=%s embed=%s lm=%s dec=%s). Add entries ")
            TEXT("under Project Settings → Plugins → InoAgents → Chatterbox → Models."),
            *ChatterboxVariantToString(EncV),
            *ChatterboxVariantToString(EmbedV),
            *ChatterboxVariantToString(LMV),
            *ChatterboxVariantToString(DecV)));
        return;
    }

    DownloadQueue    = BuildDownloadQueue(Settings, PendingConfig);
    DownloadCursor   = 0;
    bDownloadProbing = true;

    if (DownloadQueue.Num() == 0)
    {
        // Every file was already present — skip straight into the
        // load. IsConfigDownloaded should've caught this upstream, but
        // if a file landed between the check and here (unusual) we
        // handle it cleanly.
        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox StartDownload: nothing to download, all files cached"));
        FinishDownloadSuccess();
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox StartDownload: %d files to fetch (enc=%s embed=%s ")
           TEXT("lm=%s dec=%s)"),
           DownloadQueue.Num(),
           *ChatterboxVariantToString(EncV),
           *ChatterboxVariantToString(EmbedV),
           *ChatterboxVariantToString(LMV),
           *ChatterboxVariantToString(DecV));

    StartHeadProbe();
}

void UInoChatterboxTtsSubsystem::StartHeadProbe()
{
    check(IsInGameThread());

    // End of HEAD phase → switch to download phase.
    if (DownloadCursor >= DownloadQueue.Num())
    {
        // Summarise what we learned so the user can tell from the log
        // whether aggregate percent will work during the download.
        // HF's CDN redirect sometimes strips Content-Length on HEAD
        // responses — when that happens, the GET response headers
        // (via HandleDownloadHeader) fill in ExpectedBytes as each
        // file starts downloading, so we recover eventually.
        int32 KnownSizes = 0;
        int64 KnownTotal = 0;
        for (const FInoChatterboxDownloadFile& F : DownloadQueue)
        {
            if (F.ExpectedBytes > 0)
            {
                ++KnownSizes;
                KnownTotal += F.ExpectedBytes;
            }
        }
        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox HEAD phase complete — %d/%d files reported size ")
               TEXT("(sum of known=%.1f MB). %s"),
               KnownSizes, DownloadQueue.Num(),
               (double)KnownTotal / (1024.0 * 1024.0),
               KnownSizes == DownloadQueue.Num()
                   ? TEXT("Aggregate byte-weighted Percent will be exact.")
                   : TEXT("Some sizes unknown; Percent falls back to file-count with ")
                     TEXT("fractional current-file credit until GET headers arrive."));

        bDownloadProbing = false;
        DownloadCursor   = 0;
        StartNextFileDownload();
        return;
    }

    const FInoChatterboxDownloadFile& File = DownloadQueue[DownloadCursor];

    DownloadRequest = FHttpModule::Get().CreateRequest();
    DownloadRequest->SetURL(File.Url);
    DownloadRequest->SetVerb(TEXT("HEAD"));
    DownloadRequest->OnProcessRequestComplete().BindUObject(
        this, &UInoChatterboxTtsSubsystem::HandleHeadComplete);

    UE_LOG(LogInoAgents, Verbose,
           TEXT("Chatterbox HEAD %d/%d: %s"),
           DownloadCursor + 1, DownloadQueue.Num(), *File.Url);

    DownloadRequest->ProcessRequest();
}

void UInoChatterboxTtsSubsystem::HandleHeadComplete(
    FHttpRequestPtr /*Request*/, FHttpResponsePtr Response, bool bSucceeded)
{
    check(IsInGameThread());
    DownloadRequest.Reset();

    // If the download state was torn down between dispatch and this
    // callback (UnloadModels), DownloadQueue is empty and the cursor
    // is meaningless — bail silently; PendingOnLoaded has already been
    // fired by UnloadModels.
    if (DownloadQueue.Num() == 0 || DownloadCursor >= DownloadQueue.Num())
    {
        return;
    }

    FInoChatterboxDownloadFile& File = DownloadQueue[DownloadCursor];
    const int32 Code = Response.IsValid() ? Response->GetResponseCode() : 0;

    if (bSucceeded && Response.IsValid() && (Code == 200 || (Code >= 200 && Code < 400)))
    {
        // Try to read Content-Length. HuggingFace's 302 redirect sometimes
        // strips it — treat missing as "unknown", not an error.
        const FString Len = Response->GetHeader(TEXT("Content-Length"));
        if (!Len.IsEmpty())
        {
            const int64 Parsed = FCString::Atoi64(*Len);
            if (Parsed > 0)
            {
                File.ExpectedBytes = Parsed;
            }
        }
        UE_LOG(LogInoAgents, Verbose,
               TEXT("Chatterbox HEAD %d/%d: ok, ExpectedBytes=%lld"),
               DownloadCursor + 1, DownloadQueue.Num(), File.ExpectedBytes);
    }
    else if (bSucceeded && Code == 404 && !File.bRequired)
    {
        // Optional file genuinely not on the server (variant inlines
        // weights). Mark as done so the download phase skips it.
        File.bDone = true;
        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox HEAD %d/%d: 404 on optional file %s — skipping"),
               DownloadCursor + 1, DownloadQueue.Num(), *File.TargetPath);
    }
    else if (bSucceeded && Code == 404 && File.bRequired)
    {
        FinishDownloadError(FString::Printf(
            TEXT("Chatterbox download: required file 404 on HEAD: %s"),
            *File.Url));
        return;
    }
    else
    {
        // HEAD failed (network error, 5xx, etc.). Don't treat this as
        // fatal — HF's HEAD flakes sometimes. Fall through with
        // ExpectedBytes=-1; the GET will either succeed (and we report
        // progress in bytes-only mode) or fail conclusively.
        UE_LOG(LogInoAgents, Verbose,
               TEXT("Chatterbox HEAD %d/%d: non-fatal probe failure (code=%d); ")
               TEXT("continuing with unknown total size"),
               DownloadCursor + 1, DownloadQueue.Num(), Code);
    }

    ++DownloadCursor;
    StartHeadProbe();
}

void UInoChatterboxTtsSubsystem::StartNextFileDownload()
{
    check(IsInGameThread());

    // Skip already-done entries (optional 404s marked during HEAD).
    while (DownloadCursor < DownloadQueue.Num()
           && DownloadQueue[DownloadCursor].bDone)
    {
        ++DownloadCursor;
    }

    if (DownloadCursor >= DownloadQueue.Num())
    {
        FinishDownloadSuccess();
        return;
    }

    FInoChatterboxDownloadFile& File = DownloadQueue[DownloadCursor];
    File.BytesWritten = 0;

    // Open .partial for writing. If a prior aborted run left one
    // behind, OpenWrite(..., bAppend=false) truncates it — correct
    // behaviour.
    const FString PartialPath = File.TargetPath + TEXT(".partial");
    if (DownloadFileHandle != nullptr)
    {
        // Defensive — a previous file's handle should have been closed
        // in the completion handler, but never hurts to ensure.
        delete DownloadFileHandle;
        DownloadFileHandle = nullptr;
    }
    DownloadFileHandle = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*PartialPath);
    if (DownloadFileHandle == nullptr)
    {
        FinishDownloadError(FString::Printf(
            TEXT("Chatterbox download: failed to open %s for writing"),
            *PartialPath));
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox GET %d/%d: %s (%lld bytes expected)"),
           DownloadCursor + 1, DownloadQueue.Num(),
           *File.Url, File.ExpectedBytes);

    DownloadRequest = FHttpModule::Get().CreateRequest();
    DownloadRequest->SetURL(File.Url);
    DownloadRequest->SetVerb(TEXT("GET"));
    DownloadRequest->SetHeader(TEXT("Accept"), TEXT("*/*"));

    // OnHeaderReceived fires for each response header — we use it to
    // latch Content-Length into the current file's ExpectedBytes so
    // aggregate percent works even when HEAD stripped the size (HF's
    // CDN sometimes does).
    DownloadRequest->OnHeaderReceived().BindUObject(
        this, &UInoChatterboxTtsSubsystem::HandleDownloadHeader);

    // OnRequestProgress64 fires with (BytesSent, BytesReceived) during
    // the download — use it to fire OnDownloadProgress with smooth
    // per-file updates. UE 5.7 deprecated the int32 OnRequestProgress
    // in favour of uint64 OnRequestProgress64 to avoid overflow on
    // files > 2 GB.
    DownloadRequest->OnRequestProgress64().BindUObject(
        this, &UInoChatterboxTtsSubsystem::HandleDownloadProgress);
    DownloadRequest->OnProcessRequestComplete().BindUObject(
        this, &UInoChatterboxTtsSubsystem::HandleDownloadComplete);

    DownloadRequest->ProcessRequest();
}

void UInoChatterboxTtsSubsystem::HandleDownloadProgress(
    FHttpRequestPtr /*Request*/, uint64 /*BytesSent*/, uint64 BytesReceived)
{
    check(IsInGameThread());
    if (DownloadQueue.Num() == 0 || DownloadCursor >= DownloadQueue.Num())
    {
        return;
    }

    // Clamp to int64 max. No real-world HTTP response will exceed
    // that, but our BytesWritten/ExpectedBytes are int64 and we want
    // to avoid implicit narrowing warnings.
    DownloadQueue[DownloadCursor].BytesWritten =
        (int64)FMath::Min<uint64>(BytesReceived, (uint64)INT64_MAX);
    BroadcastDownloadProgress();
}

void UInoChatterboxTtsSubsystem::HandleDownloadHeader(
    FHttpRequestPtr /*Request*/,
    const FString& HeaderName,
    const FString& HeaderValue)
{
    check(IsInGameThread());
    if (DownloadQueue.Num() == 0 || DownloadCursor >= DownloadQueue.Num())
    {
        return;
    }

    // Only interested in Content-Length. Case-insensitive compare — RFC
    // says header names are case-insensitive, and in practice we see
    // both "Content-Length" and "content-length" in the wild.
    if (!HeaderName.Equals(TEXT("Content-Length"), ESearchCase::IgnoreCase))
    {
        return;
    }

    const int64 Parsed = FCString::Atoi64(*HeaderValue);
    if (Parsed <= 0)
    {
        return;
    }

    FInoChatterboxDownloadFile& File = DownloadQueue[DownloadCursor];
    if (File.ExpectedBytes > 0 && File.ExpectedBytes == Parsed)
    {
        return;   // HEAD already told us the same value; nothing to do.
    }

    if (File.ExpectedBytes <= 0)
    {
        UE_LOG(LogInoAgents, Verbose,
               TEXT("Chatterbox GET %d/%d: learned Content-Length=%lld from GET response ")
               TEXT("(HEAD didn't give us one)"),
               DownloadCursor + 1, DownloadQueue.Num(), Parsed);
    }
    File.ExpectedBytes = Parsed;

    // Refresh aggregate progress now that we have a better total.
    BroadcastDownloadProgress();
}

void UInoChatterboxTtsSubsystem::HandleDownloadComplete(
    FHttpRequestPtr /*Request*/, FHttpResponsePtr Response, bool bSucceeded)
{
    check(IsInGameThread());
    DownloadRequest.Reset();

    if (DownloadQueue.Num() == 0 || DownloadCursor >= DownloadQueue.Num())
    {
        return;   // teardown during request
    }

    FInoChatterboxDownloadFile& File = DownloadQueue[DownloadCursor];
    const int32 Code = Response.IsValid() ? Response->GetResponseCode() : 0;

    // -------- Handle 404 on optional files --------
    if (bSucceeded && Code == 404 && !File.bRequired)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox GET %d/%d: 404 on optional %s — skipping"),
               DownloadCursor + 1, DownloadQueue.Num(), *File.TargetPath);
        if (DownloadFileHandle)
        {
            delete DownloadFileHandle;
            DownloadFileHandle = nullptr;
            // Delete the empty .partial we opened speculatively.
            IFileManager::Get().Delete(*(File.TargetPath + TEXT(".partial")));
        }
        File.bDone = true;
        ++DownloadCursor;
        StartNextFileDownload();
        return;
    }

    // -------- Hard errors --------
    if (!bSucceeded || !Response.IsValid() || Code != 200)
    {
        FinishDownloadError(FString::Printf(
            TEXT("Chatterbox GET %d/%d failed: %s (HTTP %d)"),
            DownloadCursor + 1, DownloadQueue.Num(), *File.Url, Code));
        return;
    }

    // -------- Success: write + atomic rename --------
    const TArray<uint8>& Content = Response->GetContent();
    if (DownloadFileHandle == nullptr)
    {
        FinishDownloadError(TEXT("Chatterbox GET: file handle closed before write"));
        return;
    }
    if (Content.Num() > 0)
    {
        DownloadFileHandle->Write(Content.GetData(), Content.Num());
    }
    delete DownloadFileHandle;
    DownloadFileHandle = nullptr;

    // Rename .partial → final. Move with Replace=true so a stale final
    // file from a prior corrupt download gets replaced cleanly.
    const FString PartialPath = File.TargetPath + TEXT(".partial");
    if (!IFileManager::Get().Move(
            *File.TargetPath, *PartialPath, /*Replace=*/ true))
    {
        IFileManager::Get().Delete(*PartialPath);
        FinishDownloadError(FString::Printf(
            TEXT("Chatterbox GET: failed to rename %s → %s"),
            *PartialPath, *File.TargetPath));
        return;
    }

    File.BytesWritten = Content.Num();
    File.bDone        = true;
    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox GET %d/%d: OK, %lld bytes → %s"),
           DownloadCursor + 1, DownloadQueue.Num(),
           File.BytesWritten, *File.TargetPath);

    // Fire a final progress update to clock this file's contribution
    // into the aggregate before we advance the cursor.
    BroadcastDownloadProgress();

    ++DownloadCursor;
    StartNextFileDownload();
}

void UInoChatterboxTtsSubsystem::BroadcastDownloadProgress()
{
    // Sizes arrive in phases: HEAD probes first (may fail silently if
    // HF's CDN redirect strips Content-Length), then per-file GET
    // response headers (via HandleDownloadHeader, latching ExpectedBytes
    // as each GET starts). To keep the reported Percent monotonic
    // regardless of when sizes land, we run two strategies and pick
    // the best one available:
    //
    //   1. Byte-weighted (preferred). Used when every file's
    //      ExpectedBytes is known, either a priori from HEAD or latched
    //      from a completed GET. Gives an accurate percent that's
    //      proportional to real on-disk bytes.
    //
    //   2. File-count with current-file partial credit (fallback).
    //      Used when any file's size is unknown. Percent =
    //      (done_count + current_fraction) / total_count. Monotonic —
    //      each completed file bumps percent by 1/N, and the current
    //      file's fractional bump interpolates smoothly. Inaccurate
    //      in absolute terms when file sizes vary a lot, but the
    //      shape matches user expectations ("bar fills as files
    //      finish").
    //
    // AggregateReceived (real bytes on disk) is always reported
    // accurately regardless of which strategy computed Percent, so a
    // Blueprint showing "123 MB downloaded" stays correct.
    //
    // TotalBytes is reported only if strategy 1 applied (all sizes
    // known) — otherwise -1, so Blueprints know the total is unknown.

    const int32 FileCount = DownloadQueue.Num();
    if (FileCount == 0)
    {
        PendingOnDownloadProgress.ExecuteIfBound(0.0f, 0, -1, /*bCompleted=*/ false);
        return;
    }

    int64 AggregateReceived = 0;
    int64 AggregateTotalKnown = 0;
    int32 DoneFiles           = 0;
    bool  bAllSizesKnown      = true;

    for (int32 i = 0; i < FileCount; ++i)
    {
        const FInoChatterboxDownloadFile& F = DownloadQueue[i];

        if (F.bDone)
        {
            ++DoneFiles;
            AggregateReceived   += F.BytesWritten;
            // Done file's "size" is authoritative — use ExpectedBytes
            // if we had it, else BytesWritten. Either way, contributes
            // 100% of its own size to AggregateTotalKnown.
            AggregateTotalKnown += (F.ExpectedBytes > 0 ? F.ExpectedBytes : F.BytesWritten);
            continue;
        }

        // Not done. Current-file bytes-in-flight count only for
        // AggregateReceived.
        if (i == DownloadCursor)
        {
            AggregateReceived += F.BytesWritten;
        }

        if (F.ExpectedBytes > 0)
        {
            AggregateTotalKnown += F.ExpectedBytes;
        }
        else
        {
            bAllSizesKnown = false;
        }
    }

    float Percent = 0.0f;

    if (bAllSizesKnown && AggregateTotalKnown > 0)
    {
        // Strategy 1: byte-weighted. Exact.
        Percent = FMath::Clamp(
            (float)((double)AggregateReceived * 100.0 / (double)AggregateTotalKnown),
            0.0f, 100.0f);
    }
    else
    {
        // Strategy 2: file-count with partial current-file credit.
        double ProgressFiles = (double)DoneFiles;
        if (DownloadCursor < FileCount && !DownloadQueue[DownloadCursor].bDone)
        {
            const FInoChatterboxDownloadFile& Current = DownloadQueue[DownloadCursor];
            if (Current.ExpectedBytes > 0 && Current.BytesWritten > 0)
            {
                ProgressFiles += FMath::Clamp(
                    (double)Current.BytesWritten / (double)Current.ExpectedBytes,
                    0.0, 1.0);
            }
        }
        Percent = (float)FMath::Clamp(
            ProgressFiles * 100.0 / (double)FileCount,
            0.0, 100.0);
    }

    const int64 TotalReport = bAllSizesKnown ? AggregateTotalKnown : -1;

    // Intermediate progress tick — bCompleted=false. The terminal
    // bCompleted=true tick is fired inside FinishDownloadSuccess after
    // the whole queue has been atomic-renamed on disk.
    PendingOnDownloadProgress.ExecuteIfBound(
        Percent, AggregateReceived, TotalReport, /*bCompleted=*/ false);
}

void UInoChatterboxTtsSubsystem::FinishDownloadSuccess()
{
    check(IsInGameThread());

    const int32 NumFiles = DownloadQueue.Num();
    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox download complete — %d files staged"), NumFiles);

    // Terminal download-progress broadcast — bCompleted=true. Fires
    // exactly once per successful download, BEFORE the ThreadPool
    // load dispatch. UI listeners can flip state from "downloading"
    // to "loading" here without waiting for OnLoaded (session creation
    // takes another 1-5 s). Percent clamps to 100.0 for a clean final
    // tick even if the byte-weighted calculation didn't quite land
    // there due to aggregate-total estimation vs actual bytes.
    //
    // Compute aggregate-received + total from the queue before
    // CleanupDownload clears it. On download failure this broadcast
    // does NOT fire — FinishDownloadError routes the error through
    // OnLoaded(false, err) instead.
    int64 FinalReceived = 0;
    int64 FinalTotal    = 0;
    bool  bAllKnown     = true;
    for (const FInoChatterboxDownloadFile& F : DownloadQueue)
    {
        FinalReceived += F.BytesWritten;
        if (F.ExpectedBytes > 0) { FinalTotal += F.ExpectedBytes; }
        else if (F.BytesWritten > 0) { FinalTotal += F.BytesWritten; }
        else { bAllKnown = false; }
    }
    PendingOnDownloadProgress.ExecuteIfBound(
        100.0f, FinalReceived, bAllKnown ? FinalTotal : -1,
        /*bCompleted=*/ true);

    // Snapshot the config before CleanupDownload (which doesn't clear
    // PendingConfig, but keeping it explicit) so the chained
    // DispatchLoadWorker has a stable value.
    const FInoChatterboxModelConfig Config = PendingConfig;

    CleanupDownload();

    // Chain into the load flow. bLoadInFlight stays true across the
    // transition — DispatchLoadWorker's hop-back clears it when the
    // ThreadPool worker finishes.
    DispatchLoadWorker(Config);
}

void UInoChatterboxTtsSubsystem::FinishDownloadError(const FString& Err)
{
    check(IsInGameThread());
    UE_LOG(LogInoAgents, Error, TEXT("%s"), *Err);

    // Delete any dangling .partial for the file we were working on so
    // next LoadModelsAsync doesn't pick up a stale half-file. Already-
    // finished files stay on disk — they're legitimate cache entries.
    if (DownloadQueue.IsValidIndex(DownloadCursor))
    {
        IFileManager::Get().Delete(
            *(DownloadQueue[DownloadCursor].TargetPath + TEXT(".partial")));
    }

    const FOnInoChatterboxModelsLoaded OnLoadedCopy = PendingOnLoaded;
    CleanupDownload();
    bLoadInFlight = false;
    OnLoadedCopy.ExecuteIfBound(false, Err);
}

void UInoChatterboxTtsSubsystem::CleanupDownload()
{
    // Close any open file handle. The .partial file itself is left on
    // disk intentionally — FinishDownloadError deletes it, but success
    // paths hand it to the rename step before calling CleanupDownload.
    if (DownloadFileHandle != nullptr)
    {
        delete DownloadFileHandle;
        DownloadFileHandle = nullptr;
    }

    // Reset the queue state. DownloadRequest is already Reset in the
    // HTTP completion handlers; defensive Reset here too in case we
    // arrive via an error path that didn't touch it.
    DownloadRequest.Reset();
    DownloadQueue.Reset();
    DownloadCursor   = 0;
    bDownloadProbing = false;
    // NOTE: PendingConfig + PendingOnLoaded are NOT cleared here —
    // FinishDownloadSuccess needs them to chain into DispatchLoadWorker,
    // and FinishDownloadError already read PendingOnLoaded into a local
    // before calling this. They're effectively one-shot values that get
    // overwritten on the next LoadModelsAsync.
}
