// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Chatterbox/InoChatterboxTtsSubsystem.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

#include "InoAgentsLog.h"
#include "InoChatterboxModels.h"
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

    // Commit 2 scope: require the files to already be on disk. The
    // auto-download flow (HEAD-probe all 11 files + chained GETs +
    // aggregated OnDownloadProgress) lands in Commit 4. Until then,
    // devs stage files by running
    //   Plugins/InoAgents/Chatterbox/scripts/setup-chatterbox.ps1
    // which populates the exact same directory the subsystem resolves
    // to here.
    if (!IsModelDownloaded(VariantEnum))
    {
        const FString Err = FString::Printf(
            TEXT("Chatterbox variant '%s' is not staged at %s. Run ")
            TEXT("Plugins/InoAgents/Chatterbox/scripts/setup-chatterbox.ps1 ")
            TEXT("(or wait for the auto-download flow — landing in Commit 4)."),
            *VariantStr, *Dir);
        UE_LOG(LogInoAgents, Warning, TEXT("%s"), *Err);
        OnLoaded.ExecuteIfBound(false, Err);
        return;
    }

    // -------- Dispatch to ThreadPool --------
    //
    // Loading the 4 ORT sessions + parsing tokenizer.json takes 1–5 s
    // on first run (XNNPACK cache generation) and ~1 s on warm runs.
    // We never block the game thread for this.
    //
    // Capture by value (primitives, copies of the path strings, and
    // the delegate struct). WeakThis guards the final hop-back so we
    // no-op cleanly if the subsystem is torn down mid-load.

    bLoadInFlight = true;

    TWeakObjectPtr<UInoChatterboxTtsSubsystem> WeakThis(this);
    const double TStart = FPlatformTime::Seconds();

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox LoadModelsAsync: dispatching (variant=%s, dir=%s)"),
           *VariantStr, *Dir);

    Async(EAsyncExecution::ThreadPool,
          [VariantEnum, VariantStr, Dir, WeakThis, OnLoaded, TStart]()
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
            [WeakThis, OnLoaded, ElapsedMs, VariantEnum,
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
                       TEXT("Chatterbox LoadModelsAsync completion: subsystem is gone; ")
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
                       TEXT("Chatterbox LoadModelsAsync: result dropped after %.1f ms ")
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
                       TEXT("Chatterbox LoadModelsAsync: FAILED after %.1f ms: %s"),
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
            Subsys->LoadedVariant = VariantEnum;

            UE_LOG(LogInoAgents, Log,
                   TEXT("Chatterbox LoadModelsAsync: SUCCESS variant=%s in %.1f ms"),
                   *ChatterboxVariantToString(VariantEnum), ElapsedMs);
            OnLoaded.ExecuteIfBound(true, FString());
        });
    });
}

void UInoChatterboxTtsSubsystem::UnloadModels()
{
    check(IsInGameThread());

    // Resetting the TUniquePtrs destroys the 4 ORT sessions (LIFO via
    // FInoChatterboxModels's members) and frees the tokenizer's BPE
    // tables. Cheap — no native teardown cost beyond heap frees.
    //
    // Commit 3 will add: cancel the in-flight synth worker before
    // resetting, so a running AR loop can't deref the destroyed bundle.

    if (bLoadInFlight)
    {
        // A load is in flight. We cannot cancel the ThreadPool worker
        // (tokenizer / ORT session creation is synchronous), but we
        // CAN tell its game-thread hop-back to drop the result so the
        // caller actually ends up unloaded after this returns. Without
        // this flag, the hop-back would blindly assign into
        // Models/Tokenizer seconds later, silently undoing the unload.
        UE_LOG(LogInoAgents, Log,
               TEXT("UnloadModels called during in-flight load — the load's ")
               TEXT("result will be dropped when it completes"));
        bPendingUnload = true;
    }
    else if (Models.IsValid() || Tokenizer.IsValid())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("UInoChatterboxTtsSubsystem::UnloadModels — clearing variant=%s"),
               *ChatterboxVariantToString(LoadedVariant));
    }

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
    // Commit 1 stub.
    //
    // Real flow (Commit 3): guard bSynthInFlight + IsModelsLoaded, load
    // reference WAV off-thread (or use Voice.ReferenceSamples),
    // enforce 24 kHz mono, construct FInoChatterboxRunner on a worker
    // from the cached bundle+tokenizer, call SynthesizeText with a
    // CancelFlag, marshal the FSynthesisResult into an
    // FInoChatterboxSynthesisResult and fire OnComplete on the game
    // thread.
    UE_LOG(LogInoAgents, Warning,
           TEXT("UInoChatterboxTtsSubsystem::SynthesizeAsync: not yet implemented ")
           TEXT("(Commit 1 scaffolding). Text length=%d, MaxNewTokens=%d."),
           Text.Len(), Options.MaxNewTokens);

    // Suppress "unused" warnings for the Voice arg — the real code in
    // Commit 3 will consume it.
    (void)Voice;

    FInoChatterboxSynthesisResult EmptyResult;
    OnComplete.ExecuteIfBound(
        false,
        EmptyResult,
        TEXT("SynthesizeAsync is a Commit 1 stub — real synthesis lands in Commit 3."));
}

void UInoChatterboxTtsSubsystem::CancelSynthesis()
{
    // Commit 1 stub — with no worker thread running yet, there's
    // nothing to cancel. Commit 3 will set an atomic flag the AR loop
    // samples per token.
}
