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
    const FInoChatterboxModelConfig& Config,
    const FOnInoChatterboxModelsLoaded& OnLoaded)
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

    // -------- Resolve the variant directory --------

    const EInoChatterboxVariant VariantEnum = Config.Variant;
    const FString               VariantStr  = ChatterboxVariantToString(VariantEnum);
    const FString               Dir         = ChatterboxResolveVariantDir(VariantEnum);

    // Remember the config + delegate for the download flow's
    // post-download hop into DispatchLoadWorker, and (if we skip
    // download) for symmetry.
    bLoadInFlight   = true;
    PendingConfig   = Config;
    PendingOnLoaded = OnLoaded;

    // -------- Files-present fast path --------
    if (IsModelDownloaded(VariantEnum))
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox LoadModelsAsync: files present, skipping download ")
               TEXT("(variant=%s, dir=%s)"),
               *VariantStr, *Dir);
        DispatchLoadWorker(VariantEnum, Dir);
        return;
    }

    // -------- Download path --------
    // Files are missing. Look up the model entry for its HF repo URL
    // + revision and kick off the sequential HEAD-probe + GET-download
    // state machine. All subsequent state transitions happen via HTTP
    // callbacks on the game thread; LoadModelsAsync returns here.
    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox LoadModelsAsync: files missing — starting download ")
           TEXT("(variant=%s, dir=%s)"),
           *VariantStr, *Dir);

    StartDownload();
}

void UInoChatterboxTtsSubsystem::DispatchLoadWorker(
    EInoChatterboxVariant Variant, const FString& Dir)
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
    const FString                              VariantStr = ChatterboxVariantToString(Variant);
    const FOnInoChatterboxModelsLoaded         OnLoaded   = PendingOnLoaded;
    const double                               TStart     = FPlatformTime::Seconds();

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox DispatchLoadWorker: dispatching (variant=%s, dir=%s)"),
           *VariantStr, *Dir);

    Async(EAsyncExecution::ThreadPool,
          [Variant, VariantStr, Dir, WeakThis, OnLoaded, TStart]()
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
            FInoChatterboxTokenizer::LoadFromJson(
                FPaths::Combine(Dir, TEXT("tokenizer.json")),
                &LocalError);

        TUniquePtr<FInoChatterboxModels> LocalModels;
        if (LocalTokenizer.IsValid())
        {
            LocalModels = FInoChatterboxModels::LoadFromDir(
                Dir, VariantStr, &LocalError);
        }

        const double ElapsedMs = (FPlatformTime::Seconds() - TStart) * 1000.0;

        // ============== HOP BACK TO GAME THREAD ==============
        //
        // Move the two TUniquePtrs across via init-capture + MoveTemp;
        // the lambda is `mutable` so we can then MoveTemp them again
        // into the subsystem's members.
        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, OnLoaded, ElapsedMs, Variant,
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
    const FString VariantStr = ChatterboxVariantToString(Variant);
    const FString Dir        = ChatterboxResolveVariantDir(Variant);

    auto FileOkNonEmpty = [](const FString& Path) -> bool
    {
        IFileManager& Fm = IFileManager::Get();
        if (!Fm.FileExists(*Path))
        {
            return false;
        }
        // FileSize returns -1 on stat failure; zero-length stubs also
        // fail the > 0 check. Cheap enough to do for every required file.
        return Fm.FileSize(*Path) > 0;
    };

    const FString RequiredFiles[] = {
        FString::Printf(TEXT("speech_encoder_%s.onnx"),      *VariantStr),
        FString::Printf(TEXT("embed_tokens_%s.onnx"),        *VariantStr),
        FString::Printf(TEXT("language_model_%s.onnx"),      *VariantStr),
        FString::Printf(TEXT("conditional_decoder_%s.onnx"), *VariantStr),
        FString(TEXT("tokenizer.json")),
    };

    for (const FString& Name : RequiredFiles)
    {
        if (!FileOkNonEmpty(FPaths::Combine(Dir, Name)))
        {
            return false;
        }
    }
    return true;
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
    check(IsInGameThread());

    // Helper: fire failure synchronously. Used for every pre-flight
    // rejection so callers see a deterministic "same frame" failure
    // before any worker-thread work happens.
    auto FailNow = [&OnComplete](const FString& Msg)
    {
        UE_LOG(LogInoAgents, Warning, TEXT("Chatterbox SynthesizeAsync: %s"), *Msg);
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
    // Three sources, priority order (matches FInoChatterboxVoice doc):
    //   1. WavFilePath — read from disk here on the game thread. Read
    //      is ~1–10 ms for a 5–10 s reference clip (UE's file IO, no
    //      decompression beyond the int16/fp32 passthrough). Short
    //      enough that doing it synchronously is preferable to fighting
    //      the async machinery for a one-shot load at enqueue time.
    //   2. ReferenceSamples — use as-is. Must already be 24 kHz mono fp32
    //      (we can't cheaply verify the sample rate of raw samples).
    //   3. Precomputed conditioning — Phase E, errored above.
    //
    // Strict 24 kHz policy for WavFilePath: reject mismatch with a clear
    // message. Voice cloning quality is extremely sensitive to resample
    // artifacts and we would rather the developer pre-convert offline
    // than silently degrade.
    TArray<float> ReferenceAudio;
    if (!Voice.WavFilePath.IsEmpty())
    {
        int32 WavSampleRate = 0;
        FString WavError;
        if (!InoChatterbox::ReadMonoWavAsFloat32(
                Voice.WavFilePath, ReferenceAudio, WavSampleRate, &WavError))
        {
            FailNow(FString::Printf(
                TEXT("Failed to read reference WAV '%s': %s"),
                *Voice.WavFilePath, *WavError));
            return;
        }
        if (WavSampleRate != InoChatterbox::kSampleRate)
        {
            FailNow(FString::Printf(
                TEXT("Reference WAV '%s' is %d Hz; Chatterbox Turbo requires 24000 Hz. ")
                TEXT("Pre-convert your clip (e.g. Audacity → Tracks → Resample → 24000) ")
                TEXT("rather than relying on silent in-engine resampling (voice cloning ")
                TEXT("quality is extremely sensitive to resample artifacts)."),
                *Voice.WavFilePath, WavSampleRate));
            return;
        }
    }
    else if (Voice.ReferenceSamples.Num() > 0)
    {
        // Trust the caller. Raw TArray<float> carries no sample-rate
        // metadata, so we document the 24 kHz contract in the struct's
        // header and rely on them honoring it.
        ReferenceAudio = Voice.ReferenceSamples;
    }
    else
    {
        FailNow(TEXT("Voice has no reference audio — set WavFilePath or ReferenceSamples"));
        return;
    }

    if (ReferenceAudio.Num() == 0)
    {
        FailNow(TEXT("Resolved reference audio is empty"));
        return;
    }

    // -------- Enqueue --------
    //
    // FIFO across all SynthesizeAsync calls (same worker, same queue).
    // A caller firing "sentence 1", "sentence 2", "sentence 3" in the
    // same frame gets them played back in order without building its
    // own queue on top.

    FInoChatterboxSynthesisWorker::FPendingSynth Item;
    Item.Text           = Text;
    Item.ReferenceAudio = MoveTemp(ReferenceAudio);
    Item.Options        = Options;
    Item.OnComplete     = OnComplete;

    Worker->Enqueue(MoveTemp(Item));

    UE_LOG(LogInoAgents, Verbose,
           TEXT("Chatterbox SynthesizeAsync: queued (text_len=%d, max_new_tokens=%d)"),
           Text.Len(), Options.MaxNewTokens);
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
    /** Build the URL + target path + required-ness for every file the
     *  subsystem needs to fetch for a given variant. Mirrors the file
     *  list in Plugins/InoAgents/Chatterbox/scripts/setup-chatterbox.ps1
     *  exactly so dev-time and runtime populate identical directories. */
    TArray<FInoChatterboxDownloadFile> BuildDownloadQueue(
        const FInoChatterboxModelEntry& Entry,
        EInoChatterboxVariant           Variant,
        const FString&                  TargetDir)
    {
        using FFile = FInoChatterboxDownloadFile;

        // Trim any trailing slash on the repo URL so the composed URLs
        // don't end up with a double slash (HF tolerates it but it's
        // ugly in logs).
        FString RepoUrl = Entry.HuggingFaceRepoUrl;
        while (RepoUrl.EndsWith(TEXT("/"))) { RepoUrl.LeftChopInline(1); }

        const FString Rev      = Entry.Revision.IsEmpty() ? TEXT("main") : Entry.Revision;
        const FString Base     = FString::Printf(TEXT("%s/resolve/%s"), *RepoUrl, *Rev);
        const FString Variant2 = ChatterboxVariantToString(Variant);

        TArray<FFile> Queue;

        // The four ONNX graph components. Each has a paired .onnx_data
        // companion that MAY be present (spill-over weights for >2 GB
        // variants) or may 404 (tiny variants inline weights). We queue
        // both unconditionally and tolerate 404 on the _data side via
        // bRequired = false.
        static const TCHAR* const Components[] = {
            TEXT("speech_encoder"),
            TEXT("embed_tokens"),
            TEXT("language_model"),
            TEXT("conditional_decoder"),
        };
        for (const TCHAR* Comp : Components)
        {
            const FString OnnxName = FString::Printf(TEXT("%s_%s.onnx"), Comp, *Variant2);
            Queue.Add(FFile{
                /*Url*/        FString::Printf(TEXT("%s/onnx/%s"), *Base, *OnnxName),
                /*TargetPath*/ FPaths::Combine(TargetDir, OnnxName),
                /*bRequired*/  true,
            });

            const FString DataName = FString::Printf(TEXT("%s_%s.onnx_data"), Comp, *Variant2);
            Queue.Add(FFile{
                /*Url*/        FString::Printf(TEXT("%s/onnx/%s"), *Base, *DataName),
                /*TargetPath*/ FPaths::Combine(TargetDir, DataName),
                /*bRequired*/  false,   // 404 legal for variants that inline weights
            });
        }

        // Three repo-root config files. tokenizer.json is required by
        // FInoChatterboxTokenizer::LoadFromJson; the other two aren't
        // consumed at runtime by our pipeline but they're tiny, we
        // download them for self-containment so a later tool has
        // everything it needs.
        static const TCHAR* const ConfigFiles[] = {
            TEXT("tokenizer.json"),
            TEXT("config.json"),
            TEXT("generation_config.json"),
        };
        for (const TCHAR* Cfg : ConfigFiles)
        {
            Queue.Add(FFile{
                /*Url*/        FString::Printf(TEXT("%s/%s"), *Base, Cfg),
                /*TargetPath*/ FPaths::Combine(TargetDir, Cfg),
                /*bRequired*/  true,
            });
        }

        return Queue;
    }
}   // anonymous namespace

void UInoChatterboxTtsSubsystem::StartDownload()
{
    check(IsInGameThread());
    check(bLoadInFlight);

    // Look up the settings entry so we have a repo URL + revision to
    // compose per-file URLs from. FindChatterboxModel matches by
    // variant (unlike LiteRT-LM's match-by-filename), so multiple
    // entries for the same variant aren't a thing — the first match
    // wins and the user's expected to not double up.
    const UInoAgentsSettings* Settings = UInoAgentsSettings::Get();
    const FInoChatterboxModelEntry* Entry = Settings
        ? Settings->FindChatterboxModel(PendingConfig.Variant)
        : nullptr;
    if (Entry == nullptr)
    {
        FinishDownloadError(FString::Printf(
            TEXT("Chatterbox download: no Project Settings entry for variant '%s'. ")
            TEXT("Add one under Project Settings → Plugins → InoAgents → Chatterbox → Models."),
            *ChatterboxVariantToString(PendingConfig.Variant)));
        return;
    }

    const FString TargetDir = ChatterboxResolveVariantDir(PendingConfig.Variant);

    // Ensure the target directory exists. FFileHelper won't create
    // intermediate dirs on its own.
    IFileManager::Get().MakeDirectory(*TargetDir, /*Tree=*/ true);

    DownloadQueue   = BuildDownloadQueue(*Entry, PendingConfig.Variant, TargetDir);
    DownloadCursor  = 0;
    bDownloadProbing = true;

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox StartDownload: variant=%s, %d files (repo=%s, rev=%s)"),
           *ChatterboxVariantToString(PendingConfig.Variant),
           DownloadQueue.Num(),
           *Entry->HuggingFaceRepoUrl, *Entry->Revision);

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
        OnDownloadProgress.Broadcast(0.0f, 0, -1);
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

    OnDownloadProgress.Broadcast(Percent, AggregateReceived, TotalReport);
}

void UInoChatterboxTtsSubsystem::FinishDownloadSuccess()
{
    check(IsInGameThread());

    const int32 NumFiles = DownloadQueue.Num();
    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox download complete — %d files staged for variant=%s"),
           NumFiles, *ChatterboxVariantToString(PendingConfig.Variant));

    const EInoChatterboxVariant VariantEnum = PendingConfig.Variant;
    const FString               Dir         = ChatterboxResolveVariantDir(VariantEnum);

    CleanupDownload();

    // Chain into the load flow. bLoadInFlight stays true across the
    // transition — DispatchLoadWorker's hop-back clears it when the
    // ThreadPool worker finishes.
    DispatchLoadWorker(VariantEnum, Dir);
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
