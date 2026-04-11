// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmSubsystem.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmModelConfig.h"
#include "LiteRtLm/LiteRtLmTypes.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

// The native C API. Only included in this .cpp — callers of the subsystem
// never see LiteRT-LM types directly.
#include "litert/lm/engine.h"

void ULiteRtLmSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    // Zero-init members explicitly in case a fresh subsystem is ever
    // reused (hot reload in editor). Nothing expensive happens here —
    // engine loading is lazy via LoadModelAsync.
    Engine       = nullptr;
    Settings     = nullptr;
    LoadedConfig = nullptr;
    bLoadInFlight = false;

    UE_LOG(LogInoAgents, Log, TEXT("ULiteRtLmSubsystem: Initialize"));
}

void ULiteRtLmSubsystem::Deinitialize()
{
    UE_LOG(LogInoAgents, Log, TEXT("ULiteRtLmSubsystem: Deinitialize"));

    // Note: if a load is in flight at shutdown, the ThreadPool worker is
    // still running. We do NOT block waiting for it — instead, the worker's
    // completion lambda captures a TWeakObjectPtr<ULiteRtLmSubsystem> and
    // no-ops if the subsystem is gone. This means a one-time leak of the
    // engine if shutdown races an in-flight load, which is acceptable
    // because the process is dying anyway.

    UnloadModel();
    Super::Deinitialize();
}

void ULiteRtLmSubsystem::LoadModelAsync(
    const ULiteRtLmModelConfig* Config,
    const FOnLiteRtLmModelLoaded& OnLoaded)
{
    check(IsInGameThread());

    if (bLoadInFlight)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("LoadModelAsync: another load is already in flight; rejecting"));
        OnLoaded.ExecuteIfBound(false, TEXT("A model load is already in flight"));
        return;
    }

    if (Engine != nullptr)
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("LoadModelAsync: a model is already loaded; call UnloadModel first"));
        OnLoaded.ExecuteIfBound(false, TEXT("A model is already loaded; call UnloadModel first"));
        return;
    }

    if (Config == nullptr)
    {
        OnLoaded.ExecuteIfBound(false, TEXT("Config is null"));
        return;
    }

    // Resolve the model file path via IPluginManager. Done on the game
    // thread because IPluginManager access patterns assume game thread.
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
    if (!Plugin.IsValid())
    {
        OnLoaded.ExecuteIfBound(false, TEXT("IPluginManager could not locate the InoAgents plugin"));
        return;
    }

    const FString BaseDir = Plugin->GetBaseDir();
    const FString ModelPath = FPaths::Combine(BaseDir, TEXT("Models"), Config->ModelFileName);

    if (!IFileManager::Get().FileExists(*ModelPath))
    {
        const FString Err = FString::Printf(
            TEXT("Model file not found at %s. Expected to find %s under Plugins/InoAgents/Models/."),
            *ModelPath, *Config->ModelFileName);
        UE_LOG(LogInoAgents, Error, TEXT("LoadModelAsync: %s"), *Err);
        OnLoaded.ExecuteIfBound(false, Err);
        return;
    }

    // From here on we're committed to an async load. Transition state.
    bLoadInFlight = true;
    LoadedConfig  = Config;

    // Capture-by-value what the worker needs. Do NOT capture `this` or
    // `Config` directly — a TWeakObjectPtr lets us safely no-op if the
    // subsystem or config is gone by the time the load completes.
    TWeakObjectPtr<ULiteRtLmSubsystem> WeakThis(this);
    const FString                     ModelPathCopy = ModelPath;
    const ELiteRtLmBackend            BackendCopy   = Config->Backend;
    const double                      TStart        = FPlatformTime::Seconds();

    UE_LOG(LogInoAgents, Log,
           TEXT("LoadModelAsync: dispatching async load of %s (backend=%s)"),
           *ModelPathCopy, ANSI_TO_TCHAR(LiteRtLmBackendToString(BackendCopy)));

    Async(EAsyncExecution::ThreadPool,
        [ModelPathCopy, BackendCopy, WeakThis, OnLoaded, TStart]()
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

        if (NewSettings == nullptr)
        {
            LocalError = TEXT("litert_lm_engine_settings_create returned NULL");
        }
        else
        {
            NewEngine = litert_lm_engine_create(NewSettings);
            if (NewEngine == nullptr)
            {
                LocalError = TEXT("litert_lm_engine_create returned NULL (check LiteRT-LM internal logs above)");
                litert_lm_engine_settings_delete(NewSettings);
                NewSettings = nullptr;
            }
        }

        const double Elapsed = FPlatformTime::Seconds() - TStart;

        // ============== HOP BACK TO GAME THREAD ==============
        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, NewEngine, NewSettings, LocalError, Elapsed, OnLoaded]()
        {
            // Subsystem gone (game instance shutting down, or race with
            // Deinitialize). Clean up native resources and drop the result.
            if (!WeakThis.IsValid())
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("LoadModelAsync completion: subsystem is gone; freeing engine and giving up"));
                if (NewEngine)   litert_lm_engine_delete(NewEngine);
                if (NewSettings) litert_lm_engine_settings_delete(NewSettings);
                return;
            }

            ULiteRtLmSubsystem* Subsys = WeakThis.Get();
            Subsys->bLoadInFlight = false;

            if (NewEngine == nullptr)
            {
                // Load failed. Clear the config reference so a subsequent
                // retry can succeed.
                Subsys->LoadedConfig = nullptr;
                UE_LOG(LogInoAgents, Error,
                       TEXT("LoadModelAsync: FAILED after %.2f s: %s"),
                       Elapsed, *LocalError);
                OnLoaded.ExecuteIfBound(false, LocalError);
                return;
            }

            // Success.
            Subsys->Engine   = NewEngine;
            Subsys->Settings = NewSettings;
            UE_LOG(LogInoAgents, Log,
                   TEXT("LoadModelAsync: SUCCESS in %.2f s"), Elapsed);
            OnLoaded.ExecuteIfBound(true, FString());
        });
    });
}

bool ULiteRtLmSubsystem::IsModelLoaded() const
{
    return Engine != nullptr;
}

void ULiteRtLmSubsystem::UnloadModel()
{
    check(IsInGameThread());

    if (Engine != nullptr)
    {
        litert_lm_engine_delete(Engine);
        Engine = nullptr;
    }
    if (Settings != nullptr)
    {
        litert_lm_engine_settings_delete(Settings);
        Settings = nullptr;
    }
    LoadedConfig = nullptr;

    UE_LOG(LogInoAgents, Log, TEXT("ULiteRtLmSubsystem: UnloadModel complete"));
}
