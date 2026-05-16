// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/InoLiteRtLmSubsystem.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/InoLiteRtLmConversation.h"
#include "InoLiteRtLmSettings.h"
#include "LiteRtLm/InoLiteRtLmToolBase.h"
#include "LiteRtLm/InoLiteRtLmTypes.h"

// Generic file downloader — owns HTTP, .partial staging, atomic
// rename, streaming SHA-256 verification, retries, multi-connection
// range, cancellation. Replaces the per-module chunked downloader +
// VerifyAndLoad path that lived here previously.
#include "InoDownloader.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// The native C API. Only included in this .cpp — callers of the subsystem
// never see LiteRT-LM types directly.
#include "litert/lm/engine.h"

namespace
{
    // Warmup support. Runs ONE throwaway generation on a freshly-created
    // engine (on the load worker thread, never the game thread) so the
    // first real conversation doesn't pay the XNNPACK/GPU shader-compile
    // + KV-alloc cost with no UI feedback. Best-effort: any failure is
    // logged and ignored — it never fails the model load.

    struct FInoLrlWarmupCtx
    {
        LiteRtLmConversation* Conv        = nullptr;
        FEvent*               Done        = nullptr;
        bool                  bCancelSent = false;  // single-threaded:
                                                    // LiteRT-LM serialises
                                                    // a stream's callbacks
    };

    void InoLrlWarmupCallback(void* CallbackData, const char* /*chunk*/,
                              bool is_final, const char* error_msg)
    {
        auto* const Ctx = static_cast<FInoLrlWarmupCtx*>(CallbackData);
        if (Ctx == nullptr)
        {
            return;
        }
        // First callback means prefill ran + (for GPU) shaders compiled
        // and the KV cache is allocated — that is all the priming we
        // need. Cancel so we don't generate a full reply for nothing.
        if (!Ctx->bCancelSent)
        {
            Ctx->bCancelSent = true;
            if (Ctx->Conv != nullptr)
            {
                litert_lm_conversation_cancel_process(Ctx->Conv);
            }
        }
        const bool bErrored = (error_msg != nullptr && *error_msg != '\0');
        if ((is_final || bErrored) && Ctx->Done != nullptr)
        {
            Ctx->Done->Trigger();
        }
    }

    void InoLrlWarmUpEngine(LiteRtLmEngine* Engine)
    {
        if (Engine == nullptr)
        {
            return;
        }

        LiteRtLmConversationConfig* Cfg = litert_lm_conversation_config_create();
        if (Cfg == nullptr)
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("LiteRtLm: Warmup: conversation_config_create returned NULL — skipping warmup"));
            return;
        }

        LiteRtLmConversation* Conv = litert_lm_conversation_create(Engine, Cfg);
        if (Conv == nullptr)
        {
            litert_lm_conversation_config_delete(Cfg);
            UE_LOG(LogInoAgents, Warning,
                   TEXT("LiteRtLm: Warmup: conversation_create returned NULL — skipping warmup "
                        "(engine still loaded; first real send just pays the prime cost)"));
            return;
        }

        FInoLrlWarmupCtx Ctx;
        Ctx.Conv = Conv;
        Ctx.Done = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/ true);

        const double TStart = FPlatformTime::Seconds();
        const int Rc = litert_lm_conversation_send_message_stream(
            Conv,
            R"({"role":"user","content":[{"type":"text","text":"hi"}]})",
            /*extra_context=*/ nullptr,
            /*optional_args=*/ nullptr,
            &InoLrlWarmupCallback,
            /*callback_data=*/ &Ctx);

        if (Rc == 0 && Ctx.Done != nullptr)
        {
            // Bounded — warmup must NEVER hang the model load. 60 s is
            // far more than a prefill + one decode step needs even on
            // a cold GPU shader cache.
            Ctx.Done->Wait(60u * 1000u);
        }

        if (Ctx.Done != nullptr)
        {
            FPlatformProcess::ReturnSynchEventToPool(Ctx.Done);
            Ctx.Done = nullptr;
        }
        litert_lm_conversation_delete(Conv);
        litert_lm_conversation_config_delete(Cfg);

        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Warmup: engine primed in %.2f s (rc=%d)"),
               FPlatformTime::Seconds() - TStart, Rc);
    }
}

void UInoLiteRtLmSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    // Zero-init members explicitly in case a fresh subsystem is ever
    // reused (hot reload in editor). Nothing expensive happens here —
    // engine loading is lazy via LoadModelAsync.
    Engine       = nullptr;
    LoadedConfig = FInoLiteRtLmModelConfig();
    bLoadInFlight = false;

    UE_LOG(LogInoAgents, Log, TEXT("LiteRtLm: Subsystem: Initialize — subsystem ready (engine not yet loaded)"));
}

void UInoLiteRtLmSubsystem::Deinitialize()
{
    UE_LOG(LogInoAgents, Log, TEXT("LiteRtLm: Subsystem: Deinitialize — tearing down (load_in_flight=%s, engine=%s, tools=%d)"),
           bLoadInFlight ? TEXT("yes") : TEXT("no"),
           Engine != nullptr ? TEXT("present") : TEXT("null"),
           Tools.Num());

    // Note: if a load is in flight at shutdown, the ThreadPool worker is
    // still running. We do NOT block waiting for it — instead, the worker's
    // completion lambda captures a TWeakObjectPtr<UInoLiteRtLmSubsystem> and
    // no-ops if the subsystem is gone. This means a one-time leak of the
    // engine if shutdown races an in-flight load, which is acceptable
    // because the process is dying anyway.

    // Cancel any in-flight InoNodes download so it stops writing to
    // disk after the subsystem is gone. The downloader polls the
    // token between chunks + before the rename, deletes its .partial,
    // and fires its OnComplete with bSuccess=false / "Cancelled" — our
    // completion lambda's WeakThis check then drops the result silently.
    if (DownloadCancelToken.IsValid())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Download: Deinitialize — cancelling in-flight download"));
        DownloadCancelToken->Cancel();
        DownloadCancelToken.Reset();
    }

    // Release references to every registered tool so they become
    // eligible for GC along with the subsystem itself. This is not
    // strictly necessary — UE would clear the TMap as part of
    // destroying the UPROPERTY anyway — but being explicit here
    // makes the teardown order obvious in logs if a tool's own
    // destructor does anything interesting.
    Tools.Empty();

    UnloadModel();
    Super::Deinitialize();
}

void UInoLiteRtLmSubsystem::LoadModelAsync(
    const FInoLiteRtLmModelConfig&              Config,
    const FInoLiteRtLmDownloadProgressDelegate& OnDownloadProgress,
    const FOnInoLiteRtLmModelLoaded&            OnLoaded)
{
    check(IsInGameThread());

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Subsystem: LoadModelAsync called (model=%s, backend=%s, max_tokens=%d, system_message=%s, activation=%d)"),
           *Config.ModelFileName,
           ANSI_TO_TCHAR(LiteRtLmBackendToString(Config.Backend)),
           Config.MaxNumTokens,
           Config.SystemMessage.IsEmpty() ? TEXT("<none>") : TEXT("<set>"),
           static_cast<int32>(Config.ActivationType));

    if (bLoadInFlight)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("LiteRtLm: Subsystem: LoadModelAsync rejected — another load is already in flight"));
        OnLoaded.ExecuteIfBound(false, TEXT("A model load is already in flight"));
        return;
    }

    if (Engine != nullptr)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("LiteRtLm: Subsystem: LoadModelAsync rejected — a model is already loaded; call UnloadModel first"));
        OnLoaded.ExecuteIfBound(false, TEXT("A model is already loaded; call UnloadModel first"));
        return;
    }

    // Resolve settings entry. FindModel is permissive — matches by
    // either DisplayName ("Gemma 4 E2B") OR LocalFileName
    // ("gemma-4-E2B-it.litertlm"). Without an entry we have no
    // download URL and no expected SHA, so we can't proceed.
    const UInoLiteRtLmSettings* LrlSettings = UInoLiteRtLmSettings::Get();
    const FInoLiteRtLmModelEntry* Entry = LrlSettings
        ? LrlSettings->FindModel(Config.ModelFileName)
        : nullptr;

    if (Entry == nullptr)
    {
        // Two failure modes share this branch:
        //   1. ModelFileName is set but doesn't match any entry → bad name.
        //   2. ModelFileName is empty AND the Models array is empty → no
        //      models configured at all. FindModel("") returns nullptr in
        //      that case (rather than first-of-empty-array). Print a
        //      tailored message so the user knows what to do.
        const FString Err = Config.ModelFileName.IsEmpty()
            ? FString(TEXT("No models configured. Add at least one entry in Project Settings "
                           "→ Plugins → InoLiteRtLm → Models."))
            : FString::Printf(
                TEXT("Model '%s' is not in the registry. Add an entry in Project Settings "
                     "→ Plugins → InoLiteRtLm → Models (match either DisplayName or LocalFileName)."),
                *Config.ModelFileName);
        UE_LOG(LogInoAgents, Error, TEXT("LiteRtLm: Subsystem: LoadModelAsync FAILED: %s"), *Err);
        OnLoaded.ExecuteIfBound(false, Err);
        return;
    }

    // Canonicalize the runtime config to use the entry's actual filename.
    // Subsequent code paths (path resolution, runtime logging, download
    // target) all see one consistent on-disk name regardless of whether
    // the caller passed in a DisplayName or a LocalFileName.
    FInoLiteRtLmModelConfig ResolvedConfig = Config;
    if (!ResolvedConfig.ModelFileName.Equals(Entry->LocalFileName, ESearchCase::IgnoreCase))
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Subsystem: LoadModelAsync resolved '%s' via display-name match → file name '%s'"),
               *ResolvedConfig.ModelFileName, *Entry->LocalFileName);
        ResolvedConfig.ModelFileName = Entry->LocalFileName;
    }

    // Backend / platform sanity. The staged LiteRT-LM build's
    // accelerator coverage is platform-specific. Selecting a backend
    // the platform has no prebuilt for otherwise surfaces only as an
    // opaque "engine_create returned NULL" much later — fail fast with
    // an actionable message instead.
    {
        const EInoLiteRtLmBackend SelBackend = ResolvedConfig.Backend;
#if PLATFORM_IOS
        if (SelBackend != EInoLiteRtLmBackend::Cpu)
        {
            const FString Err = FString::Printf(
                TEXT("Backend '%s' is not available on iOS — the staged LiteRT-LM "
                     "iOS framework is CPU-only (no GPU/NPU accelerator is shipped). "
                     "Set FInoLiteRtLmModelConfig::Backend = Cpu for iOS builds."),
                ANSI_TO_TCHAR(LiteRtLmBackendToString(SelBackend)));
            UE_LOG(LogInoAgents, Error,
                   TEXT("LiteRtLm: Subsystem: LoadModelAsync FAILED: %s"), *Err);
            OnLoaded.ExecuteIfBound(false, Err);
            return;
        }
#endif
        if (SelBackend == EInoLiteRtLmBackend::Npu)
        {
            // Upstream NPU is "ready, not functional": no vendor
            // libLiteRtDispatch_<Vendor> ships in any staged dir. Don't
            // hard-fail (a future vendor lib could make it work), but
            // warn loudly so the inevitable engine_create failure is
            // self-explanatory.
            UE_LOG(LogInoAgents, Warning,
                   TEXT("LiteRtLm: Subsystem: NPU backend selected, but no vendor NPU "
                        "dispatch library is bundled with the staged runtime (NPU is "
                        "'ready, not functional'). engine_create will almost certainly "
                        "fail — CPU is always available as a fallback."));
        }
    }

    // Build the InoNodes download request. The downloader handles
    // HEAD probe → GET → .partial staging → atomic rename → streaming
    // SHA-256 verification all internally; cached files with matching
    // SHA short-circuit straight to the completion callback.
    const FString TargetDir = UInoLiteRtLmSettings::GetModelsDir();

    FInoDownloadRequest Req;
    Req.Url                = Entry->DownloadUrl;
    Req.SaveDirectory      = TargetDir;
    Req.FileName           = Entry->LocalFileName;
    Req.ExpectedSha256     = Entry->ExpectedSha256;
    Req.ExpectedTotalBytes = Entry->FileSizeBytes;
    Req.bSkipIfCached      = true;

    // If the download URL is empty, only proceed when the file is
    // already cached (offline-after-first-run path). Otherwise this is
    // a clear configuration error and we surface it before kicking off
    // the downloader.
    const FString TargetPath = FPaths::Combine(TargetDir, Req.FileName);
    if (Req.Url.IsEmpty() && !IFileManager::Get().FileExists(*TargetPath))
    {
        const FString Err = FString::Printf(
            TEXT("Model '%s' not found on disk and no DownloadUrl is configured "
                 "in the registry entry. Either drop the file at %s manually or "
                 "set DownloadUrl in Project Settings → Plugins → InoLiteRtLm → Models."),
            *Entry->LocalFileName, *TargetPath);
        UE_LOG(LogInoAgents, Error, TEXT("LiteRtLm: Subsystem: LoadModelAsync FAILED: %s"), *Err);
        OnLoaded.ExecuteIfBound(false, Err);
        return;
    }

    // Stash per-call delegates + ResolvedConfig for the duration of
    // the load so every progress tick + the terminal OnLoaded fires
    // through them. Cleared on terminal completion.
    PendingOnLoaded           = OnLoaded;
    PendingOnDownloadProgress = OnDownloadProgress;
    LoadedConfig              = ResolvedConfig;

    bLoadInFlight       = true;
    DownloadCancelToken = MakeShared<FInoCancellationToken, ESPMode::ThreadSafe>();

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Subsystem: LoadModelAsync dispatching InoNodes download "
                "(url=%s, target=%s, expected_sha=%s, expected_bytes=%lld)"),
           Req.Url.IsEmpty() ? TEXT("<none, cached>") : *Req.Url,
           *TargetPath,
           Req.ExpectedSha256.IsEmpty() ? TEXT("<none>") : *Req.ExpectedSha256,
           Req.ExpectedTotalBytes);

    TWeakObjectPtr<UInoLiteRtLmSubsystem> WeakThis(this);

    InoNodes::Download::DownloadFileAsync(
        Req,
        // Progress — already marshalled to the game thread by InoNodes.
        // Forward verbatim through the caller's delegate.
        [WeakThis](const FInoDownloadProgress& P)
        {
            if (UInoLiteRtLmSubsystem* Self = WeakThis.Get())
            {
                Self->PendingOnDownloadProgress.ExecuteIfBound(P);
            }
        },
        // Completion — game thread.
        [WeakThis](const FInoDownloadResult& Result)
        {
            UInoLiteRtLmSubsystem* Self = WeakThis.Get();
            if (Self == nullptr)
            {
                // Subsystem gone (Deinitialize cancelled us mid-flight).
                // Downloader has already cleaned up its .partial state.
                return;
            }

            // Download is no longer in flight — drop the cancel token
            // either way so a fresh load can claim a new one.
            Self->DownloadCancelToken.Reset();

            if (!Result.bSuccess)
            {
                Self->bLoadInFlight = false;
                Self->LoadedConfig  = FInoLiteRtLmModelConfig();
                const FString Err = FString::Printf(
                    TEXT("Model download failed for %s: %s"),
                    *Result.FileName, *Result.ErrorMessage);
                UE_LOG(LogInoAgents, Error, TEXT("LiteRtLm: Subsystem: LoadModelAsync FAILED: %s"), *Err);

                // Snapshot + clear the pending delegate before firing so
                // a re-entrant LoadModelAsync from inside the handler
                // sees a clean state.
                FOnInoLiteRtLmModelLoaded Cb = Self->PendingOnLoaded;
                Self->PendingOnLoaded = FOnInoLiteRtLmModelLoaded();
                Self->PendingOnDownloadProgress = FInoLiteRtLmDownloadProgressDelegate();
                Cb.ExecuteIfBound(false, Err);
                return;
            }

            // Success. File is on disk + (if SHA configured) verified.
            // Hand off to ThreadPool for engine_create.
            UE_LOG(LogInoAgents, Log,
                   TEXT("LiteRtLm: Subsystem: download/cache OK (path=%s, %lld bytes, sha_verified=%s); loading engine..."),
                   *Result.AbsolutePath, Result.SizeBytes,
                   Result.bSha256Verified ? TEXT("yes") : TEXT("skipped"));
            Self->DispatchModelLoad(Result.AbsolutePath);
        },
        DownloadCancelToken);
}

void UInoLiteRtLmSubsystem::DispatchModelLoad(const FString& ModelPath)
{
    check(IsInGameThread());

    TWeakObjectPtr<UInoLiteRtLmSubsystem> WeakThis(this);
    const FString                       ModelPathCopy   = FPaths::ConvertRelativePathToFull(ModelPath);
    const EInoLiteRtLmBackend              BackendCopy     = LoadedConfig.Backend;
    const int32                         MaxNumTokens    = LoadedConfig.MaxNumTokens;
    const EInoLiteRtLmActivationType       ActivationType  = LoadedConfig.ActivationType;
    const FString                       CacheDirCopy    = LoadedConfig.CacheDir;
    const bool                          WarmUpCopy      = LoadedConfig.bWarmUpOnLoad;
    const double                        TStart          = FPlatformTime::Seconds();

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Engine: dispatching async engine_create of %s (backend=%s, activation=%d, max_tokens=%d)"),
           *ModelPathCopy, ANSI_TO_TCHAR(LiteRtLmBackendToString(BackendCopy)),
           static_cast<int32>(ActivationType), MaxNumTokens);

    Async(EAsyncExecution::ThreadPool,
        [ModelPathCopy, BackendCopy, MaxNumTokens, ActivationType,
         CacheDirCopy, WarmUpCopy, WeakThis, TStart]()
    {
        // ============== WORKER THREAD ==============
        //
        // Safety rules:
        //   - Do NOT touch WeakThis here (except to pass it forward to the
        //     game-thread lambda). Weak pointer access is only valid from
        //     the game thread.
        //   - Do NOT UE_LOG from here. LogInoAgents IS thread-safe in
        //     principle, but we keep the worker silent so all logs come
        //     through the game-thread lambda below (single-threaded log
        //     order preserves readability).

        const FTCHARToUTF8 ModelPathUtf8(*ModelPathCopy);
        const char* const  BackendStr = LiteRtLmBackendToString(BackendCopy);

        LiteRtLmEngineSettings* NewSettings = litert_lm_engine_settings_create(
            ModelPathUtf8.Get(), BackendStr,
            /*vision_backend_str=*/ nullptr,
            /*audio_backend_str=*/ nullptr);

        FString          LocalError;
        LiteRtLmEngine*  NewEngine = nullptr;

        if (NewSettings != nullptr)
        {
            // Apply engine-level settings from the model config.
            if (MaxNumTokens > 0)
            {
                litert_lm_engine_settings_set_max_num_tokens(NewSettings, MaxNumTokens);
            }
            // Set activation precision (F32, F16, I16, I8). F16 halves
            // runtime memory with minimal quality loss on Gemma 4.
            litert_lm_engine_settings_set_activation_data_type(
                NewSettings, static_cast<int>(ActivationType));
            if (!CacheDirCopy.IsEmpty())
            {
                const FTCHARToUTF8 CacheDirUtf8(*CacheDirCopy);
                litert_lm_engine_settings_set_cache_dir(NewSettings, CacheDirUtf8.Get());
            }
        }

        if (NewSettings == nullptr)
        {
            LocalError = TEXT("litert_lm_engine_settings_create returned NULL");
        }
        else
        {
            NewEngine = litert_lm_engine_create(NewSettings);

            // Settings are consumed synchronously by engine_create:
            // litert_lm_engine_create deep-copies *settings before
            // handing it to EngineFactory::CreateDefault, so the
            // resulting Engine carries everything forward. Free the
            // settings handle now on BOTH branches; nothing past this
            // point references it.
            litert_lm_engine_settings_delete(NewSettings);
            NewSettings = nullptr;

            if (NewEngine == nullptr)
            {
                LocalError = FString::Printf(
                    TEXT("litert_lm_engine_create returned NULL for backend '%s'. %s"
                         "Check the LiteRT-LM stderr capture above for the real cause "
                         "(corrupt/incompatible model, out of memory, or this "
                         "platform's staged build has no '%s' accelerator). CPU is "
                         "always available."),
                    ANSI_TO_TCHAR(LiteRtLmBackendToString(BackendCopy)),
                    BackendCopy != EInoLiteRtLmBackend::Cpu
                        ? TEXT("A non-CPU backend was requested. ") : TEXT(""),
                    ANSI_TO_TCHAR(LiteRtLmBackendToString(BackendCopy)));
            }
        }

        // Optional prime — still on the worker thread, before we hop
        // back to the game thread, so OnLoaded(true) only fires once
        // the engine is hot. Best-effort: never promotes to a load
        // failure.
        if (NewEngine != nullptr && WarmUpCopy)
        {
            InoLrlWarmUpEngine(NewEngine);
        }

        const double Elapsed = FPlatformTime::Seconds() - TStart;

        // ============== HOP BACK TO GAME THREAD ==============
        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, NewEngine, LocalError, Elapsed, BackendCopy]()
        {
            // Subsystem gone (game instance shutting down, or race with
            // Deinitialize). Clean up native resources and drop the result.
            if (!WeakThis.IsValid())
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("LiteRtLm: Engine: engine_create completion — subsystem is gone; freeing engine and giving up"));
                if (NewEngine) litert_lm_engine_delete(NewEngine);
                return;
            }

            UInoLiteRtLmSubsystem* Subsys = WeakThis.Get();
            Subsys->bLoadInFlight = false;
            UE_LOG(LogInoAgents, Verbose,
                   TEXT("LiteRtLm: Subsystem: bLoadInFlight → false (engine_create returned)"));

            // Snapshot + clear the pending delegate before firing so a
            // re-entrant LoadModelAsync from inside the handler sees a
            // clean state.
            FOnInoLiteRtLmModelLoaded Cb = Subsys->PendingOnLoaded;
            Subsys->PendingOnLoaded = FOnInoLiteRtLmModelLoaded();
            Subsys->PendingOnDownloadProgress = FInoLiteRtLmDownloadProgressDelegate();

            if (NewEngine == nullptr)
            {
                // Load failed. Clear the config reference so a subsequent
                // retry can succeed.
                Subsys->LoadedConfig = FInoLiteRtLmModelConfig();
                UE_LOG(LogInoAgents, Error,
                       TEXT("LiteRtLm: Engine: engine_create FAILED after %.2f s: %s"),
                       Elapsed, *LocalError);
                Cb.ExecuteIfBound(false, LocalError);
                return;
            }

            // Success.
            Subsys->Engine = NewEngine;
            UE_LOG(LogInoAgents, Log,
                   TEXT("LiteRtLm: Engine: engine_create SUCCESS in %.2f s (backend=%s)"),
                   Elapsed, ANSI_TO_TCHAR(LiteRtLmBackendToString(BackendCopy)));
            Cb.ExecuteIfBound(true, FString());
        });
    });
}

bool UInoLiteRtLmSubsystem::IsModelLoaded() const
{
    return Engine != nullptr;
}

bool UInoLiteRtLmSubsystem::IsModelDownloaded(const FString& ModelNameOrFileName) const
{
    if (ModelNameOrFileName.IsEmpty())
    {
        return false;
    }

    // Canonicalize DisplayName → filename the same way LoadModelAsync does.
    // If settings aren't available (shouldn't happen at runtime, but guard
    // anyway) or the name isn't registered, fall through with the raw input.
    FString FileName = ModelNameOrFileName;
    if (const UInoLiteRtLmSettings* LrlSettings = UInoLiteRtLmSettings::Get())
    {
        if (const FInoLiteRtLmModelEntry* Entry = LrlSettings->FindModel(ModelNameOrFileName))
        {
            FileName = Entry->LocalFileName;
        }
    }

    // LiteRtLmResolveModelPath returns empty iff the file is absent from
    // both PersistentDownloadDir and the plugin's legacy Models/ dir. It
    // already looks for the exact final filename, so any lingering
    // `<name>.partial` from an interrupted download is implicitly ignored.
    const FString ResolvedPath = LiteRtLmResolveModelPath(FileName);
    if (ResolvedPath.IsEmpty())
    {
        return false;
    }

    // Defense in depth: make sure the file is non-empty. FileSize returns
    // INDEX_NONE on error or for directories; only a strictly positive
    // size counts as "downloaded".
    const int64 Size = IFileManager::Get().FileSize(*ResolvedPath);
    return Size > 0;
}

void UInoLiteRtLmSubsystem::UnloadModel()
{
    check(IsInGameThread());

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Subsystem: UnloadModel called (engine=%s, active_conversation=%s)"),
           Engine != nullptr ? TEXT("present") : TEXT("null"),
           ActiveConversation.IsValid() ? TEXT("yes") : TEXT("no"));

    // Tear down the active conversation (if any) BEFORE the engine. LiteRT-LM
    // sessions hold raw pointers into the engine's LlmExecutor; deleting the
    // engine first and the conversation later causes ~SessionBasic to AV when
    // GC eventually runs BeginDestroy on the conversation UObject.
    if (UInoLiteRtLmConversation* Conv = ActiveConversation.Get())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Subsystem: UnloadModel shutting down active conversation before engine teardown"));
        Conv->Shutdown();
    }
    ActiveConversation.Reset();

    if (Engine != nullptr)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Engine: engine_delete — destroying native engine"));
        litert_lm_engine_delete(Engine);
        Engine = nullptr;
    }
    LoadedConfig = FInoLiteRtLmModelConfig();

    UE_LOG(LogInoAgents, Log, TEXT("LiteRtLm: Subsystem: UnloadModel complete"));
}

UInoLiteRtLmConversation* UInoLiteRtLmSubsystem::CreateConversation()
{
    return CreateConversationWithHistory(TArray<FInoLiteRtLmMessage>());
}

UInoLiteRtLmConversation* UInoLiteRtLmSubsystem::CreateConversationWithHistory(
    const TArray<FInoLiteRtLmMessage>& InitialMessages)
{
    check(IsInGameThread());

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Subsystem: CreateConversation called (initial_messages=%d, tools_registered=%d)"),
           InitialMessages.Num(), Tools.Num());

    if (Engine == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LiteRtLm: Subsystem: CreateConversation FAILED — no model loaded; call LoadModelAsync first"));
        return nullptr;
    }

    if (LoadedConfig.ModelFileName.IsEmpty())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LiteRtLm: Subsystem: CreateConversation FAILED — LoadedConfig is null despite Engine being "
                    "set (this should be impossible)"));
        return nullptr;
    }

    // Enforce single-conversation invariant. LiteRT-LM sessions on one engine
    // share a single LlmExecutor + KV cache, so a second live conversation
    // would corrupt the first. Shutdown() is synchronous: it cancels any
    // in-flight stream, joins the worker thread, and destroys the native
    // conversation.
    if (UInoLiteRtLmConversation* Prior = ActiveConversation.Get())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("LiteRtLm: Subsystem: CreateConversation — a prior conversation is still active; "
                    "shutting it down to honour the single-conversation-per-engine "
                    "invariant. Callers holding a reference to it will see "
                    "SendMessageAsync error out."));
        Prior->Shutdown();
    }
    ActiveConversation.Reset();

    UInoLiteRtLmConversation* Conv = NewObject<UInoLiteRtLmConversation>();
    Conv->Initialize(this, Engine, LoadedConfig, InitialMessages);
    ActiveConversation = Conv;
    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Subsystem: CreateConversation done (conversation=%s)"),
           *Conv->GetName());
    return Conv;
}

// ----------------------------------------------------------------------
// Tool registry (D.4)
// ----------------------------------------------------------------------

void UInoLiteRtLmSubsystem::RegisterTool(UInoLiteRtLmToolBase* Tool)
{
    check(IsInGameThread());

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Tool: RegisterTool called (tool_object=%s)"),
           Tool != nullptr ? *Tool->GetName() : TEXT("<null>"));

    if (Tool == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LiteRtLm: Tool: RegisterTool FAILED — the supplied Tool is null"));
        return;
    }

    const FName DeclaredName = Tool->ToolName;
    if (DeclaredName == NAME_None)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LiteRtLm: Tool: RegisterTool FAILED — tool %s has an empty ToolName "
                    "(every tool must have a unique non-empty name)"),
               *Tool->GetName());
        return;
    }

    // Validate the schema built from the tool's properties. It must
    // parse as JSON and its function.name must match ToolName.
    const FString SchemaJson = Tool->BuildSchemaJson();
    if (SchemaJson.IsEmpty())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LiteRtLm: Tool: RegisterTool FAILED — tool %s produced an empty schema from BuildSchemaJson"),
               *DeclaredName.ToString());
        return;
    }

    TSharedPtr<FJsonObject> SchemaObj;
    const TSharedRef<TJsonReader<>> SchemaReader = TJsonReaderFactory<>::Create(SchemaJson);
    if (!FJsonSerializer::Deserialize(SchemaReader, SchemaObj) || !SchemaObj.IsValid())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LiteRtLm: Tool: RegisterTool FAILED — tool %s schema does not parse as JSON. Schema was: %s"),
               *DeclaredName.ToString(), *SchemaJson);
        return;
    }

    // Verify function.name matches the declared ToolName.
    const TSharedPtr<FJsonObject>* FunctionObjPtr = nullptr;
    if (SchemaObj->TryGetObjectField(TEXT("function"), FunctionObjPtr)
        && FunctionObjPtr != nullptr
        && FunctionObjPtr->IsValid())
    {
        FString SchemaName;
        if ((*FunctionObjPtr)->TryGetStringField(TEXT("name"), SchemaName))
        {
            if (FName(*SchemaName) != DeclaredName)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("LiteRtLm: Tool: RegisterTool FAILED — tool %s schema name mismatch: "
                            "ToolName==%s but schema function.name==%s. Refusing to register."),
                       *Tool->GetName(), *DeclaredName.ToString(), *SchemaName);
                return;
            }
        }
    }

    if (Tools.Contains(DeclaredName))
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("LiteRtLm: Tool: RegisterTool replacing existing registration for tool \"%s\""),
               *DeclaredName.ToString());
    }

    Tools.Add(DeclaredName, Tool);

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Tool: registered \"%s\" (schema %d bytes, params=%d, total_tools=%d)"),
           *DeclaredName.ToString(), SchemaJson.Len(), Tool->Parameters.Num(), Tools.Num());
}

void UInoLiteRtLmSubsystem::UnregisterTool(FName ToolName)
{
    check(IsInGameThread());

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Tool: UnregisterTool called (name=\"%s\")"),
           *ToolName.ToString());

    const int32 NumRemoved = Tools.Remove(ToolName);
    if (NumRemoved == 0)
    {
        UE_LOG(LogInoAgents, Verbose,
               TEXT("LiteRtLm: Tool: UnregisterTool no-op — no tool registered under name \"%s\""),
               *ToolName.ToString());
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Tool: unregistered \"%s\" (total_tools=%d)"),
           *ToolName.ToString(), Tools.Num());
}

UInoLiteRtLmToolBase* UInoLiteRtLmSubsystem::FindTool(FName ToolName) const
{
    if (const TObjectPtr<UInoLiteRtLmToolBase>* Found = Tools.Find(ToolName))
    {
        return *Found;
    }
    return nullptr;
}

FString UInoLiteRtLmSubsystem::BuildToolsJsonForConversation() const
{
    check(IsInGameThread());

    if (Tools.Num() == 0)
    {
        return FString();
    }

    TArray<TSharedPtr<FJsonValue>> SchemaArray;
    SchemaArray.Reserve(Tools.Num());

    for (const TPair<FName, TObjectPtr<UInoLiteRtLmToolBase>>& Pair : Tools)
    {
        UInoLiteRtLmToolBase* const Tool = Pair.Value;
        if (Tool == nullptr)
        {
            continue;
        }

        const FString SchemaJson = Tool->BuildSchemaJson();

        TSharedPtr<FJsonObject> SchemaObj;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SchemaJson);
        if (!FJsonSerializer::Deserialize(Reader, SchemaObj) || !SchemaObj.IsValid())
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("LiteRtLm: Tool: BuildToolsJsonForConversation — tool \"%s\" schema does not parse; dropping"),
                   *Pair.Key.ToString());
            continue;
        }

        SchemaArray.Add(MakeShared<FJsonValueObject>(SchemaObj));
    }

    if (SchemaArray.Num() == 0)
    {
        return FString();
    }

    FString OutJson;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
    FJsonSerializer::Serialize(SchemaArray, Writer);

    return OutJson;
}
