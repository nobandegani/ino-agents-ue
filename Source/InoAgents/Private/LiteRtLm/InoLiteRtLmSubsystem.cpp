// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/InoLiteRtLmSubsystem.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/InoLiteRtLmConversation.h"
#include "InoAgentsSettings.h"
#include "LiteRtLm/InoLiteRtLmToolBase.h"
#include "LiteRtLm/InoLiteRtLmTypes.h"
#include "LiteRtLm/InoSha256.h"
#include "UI/Slate/InoChatBridge.h"
#include "UI/Slate/SInoChatPanel.h"

#include "Async/Async.h"
#include "HAL/PlatformFileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameViewportClient.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Layout/SBox.h"

#if WITH_EDITOR
    #include "Editor.h"
#endif

// The native C API. Only included in this .cpp — callers of the subsystem
// never see LiteRT-LM types directly.
#include "litert/lm/engine.h"

void UInoLiteRtLmSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    // Zero-init members explicitly in case a fresh subsystem is ever
    // reused (hot reload in editor). Nothing expensive happens here —
    // engine loading is lazy via LoadModelAsync.
    Engine       = nullptr;
    Settings     = nullptr;
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

    // Cancel any in-flight download so we don't write to a file after
    // the subsystem is gone.
    if (DownloadRequest.IsValid())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Download: Deinitialize — cancelling in-flight download request"));
        DownloadRequest->CancelRequest();
        DownloadRequest.Reset();
    }
    CleanupDownload();

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
    const FInoLiteRtLmModelConfig&     Config,
    const FOnInoModelDownloadProgress& OnDownloadProgress,
    const FOnInoLiteRtLmModelLoaded&   OnLoaded)
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

    // Stash the per-call progress delegate so every progress tick
    // during download routes through it. Cleared on CleanupDownload
    // so a stale delegate from a prior load can't fire against a
    // subsequent load. Pre-flight rejections (above) never set this,
    // which is fine — they failed before any download state was set up.
    PendingOnDownloadProgress = OnDownloadProgress;

    // Resolve settings entry first. FindModel is permissive — it matches
    // by either ModelFileName ("gemma-4-E2B-it.litertlm") OR DisplayName
    // ("Gemma 4 E2B"). If the caller passed a display name, we transparently
    // canonicalize the file name in a local config copy so the rest of
    // this function (disk check, download target path, download callback's
    // rename step, smoke-test paths) all use one consistent on-disk name.
    const UInoAgentsSettings* AgentSettings = UInoAgentsSettings::Get();
    const FInoLiteRtLmModelEntry* Entry = AgentSettings
        ? AgentSettings->FindModel(Config.ModelFileName)
        : nullptr;

    FInoLiteRtLmModelConfig ResolvedConfig = Config;
    if (Entry != nullptr &&
        !ResolvedConfig.ModelFileName.Equals(Entry->ModelFileName, ESearchCase::IgnoreCase))
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Subsystem: LoadModelAsync resolved '%s' via display-name match → file name '%s'"),
               *ResolvedConfig.ModelFileName, *Entry->ModelFileName);
        ResolvedConfig.ModelFileName = Entry->ModelFileName;
    }

    // Resolve the model file path. Checks PersistentDownloadDir first
    // (downloaded/cached), then the plugin's Models/ dir (legacy dev).
    const FString ModelPath = LiteRtLmResolveModelPath(ResolvedConfig.ModelFileName);

    if (!ModelPath.IsEmpty())
    {
        // Found on disk — verify SHA-256 first (if the entry has one configured),
        // then either proceed to load or delete+redownload on mismatch.
        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Subsystem: LoadModelAsync found cached model on disk (path=%s); proceeding to verify+load"),
               *ModelPath);
        bLoadInFlight = true;
        UE_LOG(LogInoAgents, Verbose,
               TEXT("LiteRtLm: Subsystem: bLoadInFlight → true (cached path)"));
        LoadedConfig  = ResolvedConfig;
        VerifyAndLoad(ModelPath, Entry, OnLoaded, /*bAllowRedownloadOnMismatch=*/ true);
        return;
    }

    // Not on disk — we need the settings entry's download URL.
    if (Entry == nullptr || Entry->DownloadUrl.IsEmpty())
    {
        const FString Err = FString::Printf(
            TEXT("Model '%s' not found on disk and no download URL configured. "
                 "Add an entry in Project Settings → Plugins → InoAgents → "
                 "LiteRT-LM → Models (match either the Display Name or the "
                 "Model File Name)."),
            *ResolvedConfig.ModelFileName);
        UE_LOG(LogInoAgents, Error, TEXT("LiteRtLm: Subsystem: LoadModelAsync FAILED: %s"), *Err);
        OnLoaded.ExecuteIfBound(false, Err);
        return;
    }

    // Download, then load.
    bLoadInFlight = true;
    UE_LOG(LogInoAgents, Verbose,
           TEXT("LiteRtLm: Subsystem: bLoadInFlight → true (download path)"));
    LoadedConfig  = ResolvedConfig;

    const FString TargetDir = FPaths::Combine(
        FPaths::ProjectPersistentDownloadDir(),
        TEXT("InoAgents"), TEXT("Models"));
    IFileManager::Get().MakeDirectory(*TargetDir, /*Tree=*/true);

    const FString TargetPath = FPaths::Combine(TargetDir, ResolvedConfig.ModelFileName);

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Subsystem: LoadModelAsync model not found locally, downloading from %s → %s"),
           *Entry->DownloadUrl, *TargetPath);

    StartDownload(Entry->DownloadUrl, TargetPath, OnLoaded);
}

void UInoLiteRtLmSubsystem::ProceedWithLoad(
    const FString& ModelPath, const FOnInoLiteRtLmModelLoaded& OnLoaded)
{
    TWeakObjectPtr<UInoLiteRtLmSubsystem> WeakThis(this);
    const FString                       ModelPathCopy   = ModelPath;
    const EInoLiteRtLmBackend              BackendCopy     = LoadedConfig.Backend;
    const int32                         MaxNumTokens    = LoadedConfig.MaxNumTokens;
    const EInoLiteRtLmActivationType       ActivationType  = LoadedConfig.ActivationType;
    const FString                       CacheDirCopy    = LoadedConfig.CacheDir;
    const double                        TStart          = FPlatformTime::Seconds();

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Engine: dispatching async engine_create of %s (backend=%s, activation=%d, max_tokens=%d)"),
           *ModelPathCopy, ANSI_TO_TCHAR(LiteRtLmBackendToString(BackendCopy)),
           static_cast<int32>(ActivationType), MaxNumTokens);

    Async(EAsyncExecution::ThreadPool,
        [ModelPathCopy, BackendCopy, MaxNumTokens, ActivationType,
         CacheDirCopy, WeakThis, OnLoaded, TStart]()
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
            [WeakThis, NewEngine, NewSettings, LocalError, Elapsed, OnLoaded, BackendCopy]()
        {
            // Subsystem gone (game instance shutting down, or race with
            // Deinitialize). Clean up native resources and drop the result.
            if (!WeakThis.IsValid())
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("LiteRtLm: Engine: engine_create completion — subsystem is gone; freeing engine and giving up"));
                if (NewEngine)   litert_lm_engine_delete(NewEngine);
                if (NewSettings) litert_lm_engine_settings_delete(NewSettings);
                return;
            }

            UInoLiteRtLmSubsystem* Subsys = WeakThis.Get();
            Subsys->bLoadInFlight = false;
            UE_LOG(LogInoAgents, Verbose,
                   TEXT("LiteRtLm: Subsystem: bLoadInFlight → false (engine_create returned)"));

            if (NewEngine == nullptr)
            {
                // Load failed. Clear the config reference so a subsequent
                // retry can succeed.
                Subsys->LoadedConfig = FInoLiteRtLmModelConfig();
                UE_LOG(LogInoAgents, Error,
                       TEXT("LiteRtLm: Engine: engine_create FAILED after %.2f s: %s"),
                       Elapsed, *LocalError);
                OnLoaded.ExecuteIfBound(false, LocalError);
                return;
            }

            // Success.
            Subsys->Engine   = NewEngine;
            Subsys->Settings = NewSettings;
            UE_LOG(LogInoAgents, Log,
                   TEXT("LiteRtLm: Engine: engine_create SUCCESS in %.2f s (backend=%s)"),
                   Elapsed, ANSI_TO_TCHAR(LiteRtLmBackendToString(BackendCopy)));
            OnLoaded.ExecuteIfBound(true, FString());
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
    if (const UInoAgentsSettings* AgentSettings = UInoAgentsSettings::Get())
    {
        if (const FInoLiteRtLmModelEntry* Entry = AgentSettings->FindModel(ModelNameOrFileName))
        {
            FileName = Entry->ModelFileName;
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
    if (Settings != nullptr)
    {
        litert_lm_engine_settings_delete(Settings);
        Settings = nullptr;
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

// ======================================================================
// Model download
// ======================================================================

void UInoLiteRtLmSubsystem::StartDownload(
    const FString& Url, const FString& TargetPath,
    const FOnInoLiteRtLmModelLoaded& OnLoaded)
{
    DownloadUrl               = Url;
    PendingDownloadTargetPath = TargetPath;
    PendingOnLoaded           = OnLoaded;
    DownloadBytesWritten      = 0;
    DownloadTotalBytes        = -1;  // learned from the first response

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Download: StartDownload (url=%s, target=%s, chunk_size=%lld bytes)"),
           *Url, *TargetPath, kDownloadChunkSize);

    // Open .partial temp file. A crash mid-download won't leave a
    // corrupt file that ResolveModelPath would find.
    const FString PartialPath = TargetPath + TEXT(".partial");
    DownloadFileHandle = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*PartialPath);
    if (DownloadFileHandle == nullptr)
    {
        const FString Err = FString::Printf(
            TEXT("Failed to open %s for writing"), *PartialPath);
        FinishDownloadError(Err);
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Download: starting chunked download (%lld-byte chunks, partial=%s)"),
           kDownloadChunkSize, *PartialPath);

    DownloadNextChunk();
}

void UInoLiteRtLmSubsystem::DownloadNextChunk()
{
    const int64 RangeStart = DownloadBytesWritten;
    const int64 RangeEnd   = RangeStart + kDownloadChunkSize - 1;

    UE_LOG(LogInoAgents, Verbose,
           TEXT("LiteRtLm: Download: requesting chunk bytes=%lld-%lld"),
           RangeStart, RangeEnd);

    DownloadRequest = FHttpModule::Get().CreateRequest();
    DownloadRequest->SetURL(DownloadUrl);
    DownloadRequest->SetVerb(TEXT("GET"));
    DownloadRequest->SetHeader(TEXT("Accept"), TEXT("*/*"));
    DownloadRequest->SetHeader(TEXT("Range"),
        FString::Printf(TEXT("bytes=%lld-%lld"), RangeStart, RangeEnd));

    DownloadRequest->OnProcessRequestComplete().BindUObject(
        this, &UInoLiteRtLmSubsystem::HandleChunkComplete);

    DownloadRequest->ProcessRequest();
}

void UInoLiteRtLmSubsystem::HandleChunkComplete(
    FHttpRequestPtr /*Request*/, FHttpResponsePtr Response, bool bSucceeded)
{
    DownloadRequest.Reset();

    if (!bSucceeded || !Response.IsValid())
    {
        FinishDownloadError(TEXT("Model download failed (network error)"));
        return;
    }

    const int32 Code = Response->GetResponseCode();

    // 416 Range Not Satisfiable = we've gone past the end of the file.
    // This means the previous chunk was the last one — we're done.
    if (Code == 416)
    {
        FinishDownloadSuccess();
        return;
    }

    // Accept 200 (server ignores Range and returns the full file — only
    // works if the file is < 2 GB) and 206 (partial content — expected
    // for chunked downloads of large files).
    if (Code != 200 && Code != 206)
    {
        FinishDownloadError(FString::Printf(TEXT("Model download failed: HTTP %d"), Code));
        return;
    }

    // Write this chunk to disk.
    const TArray<uint8>& Content = Response->GetContent();
    if (Content.Num() > 0 && DownloadFileHandle != nullptr)
    {
        DownloadFileHandle->Write(Content.GetData(), Content.Num());
        DownloadBytesWritten += Content.Num();
    }

    // Learn the full file size from the first response we can parse it from.
    // HTTP 206 (Partial Content): Content-Range header carries
    //   "bytes <start>-<end>/<total>" (or "/<*>" if unknown). We want <total>.
    // HTTP 200 (server ignored Range): Content-Length IS the full file size.
    if (DownloadTotalBytes < 0)
    {
        if (Code == 206)
        {
            const FString Range = Response->GetHeader(TEXT("Content-Range"));
            int32         SlashIdx = INDEX_NONE;
            if (Range.FindLastChar(TEXT('/'), SlashIdx))
            {
                const FString TotalStr = Range.Mid(SlashIdx + 1).TrimStartAndEnd();
                if (!TotalStr.IsEmpty() && TotalStr != TEXT("*"))
                {
                    const int64 Parsed = FCString::Atoi64(*TotalStr);
                    if (Parsed > 0)
                    {
                        DownloadTotalBytes = Parsed;
                    }
                }
            }
        }
        else if (Code == 200)
        {
            const FString Len = Response->GetHeader(TEXT("Content-Length"));
            if (!Len.IsEmpty())
            {
                const int64 Parsed = FCString::Atoi64(*Len);
                if (Parsed > 0)
                {
                    DownloadTotalBytes = Parsed;
                }
            }
        }

        if (DownloadTotalBytes > 0)
        {
            UE_LOG(LogInoAgents, Log,
                   TEXT("LiteRtLm: Download: full download size advertised as %lld bytes (%.1f MB)"),
                   DownloadTotalBytes, DownloadTotalBytes / (1024.0 * 1024.0));
        }
    }

    // Broadcast progress with real values when we know them. Clamp Percent to
    // [0, 100] defensively — DownloadBytesWritten should never exceed
    // DownloadTotalBytes under normal operation, but a mis-advertised total
    // shouldn't make the delegate report 102% to UI code.
    float Percent = 0.0f;
    if (DownloadTotalBytes > 0)
    {
        Percent = FMath::Clamp(
            static_cast<float>(static_cast<double>(DownloadBytesWritten) * 100.0 /
                               static_cast<double>(DownloadTotalBytes)),
            0.0f, 100.0f);
    }
    // Intermediate progress tick — bCompleted=false. The final
    // bCompleted=true tick is fired inside FinishDownloadSuccess
    // once the last chunk has been written to disk, so UI can flip
    // state cleanly without waiting for the engine-load phase.
    PendingOnDownloadProgress.ExecuteIfBound(
        Percent, DownloadBytesWritten, DownloadTotalBytes, /*bCompleted=*/ false);

    if (DownloadTotalBytes > 0)
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Download: progress %lld / %lld MB (%.1f%%)"),
               DownloadBytesWritten / (1024 * 1024),
               DownloadTotalBytes   / (1024 * 1024),
               Percent);
    }
    else
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("LiteRtLm: Download: progress %lld MB so far (total unknown)"),
               DownloadBytesWritten / (1024 * 1024));
    }

    // If we got a 200 (full file) or the chunk was smaller than
    // what we asked for, we're done — this was the last chunk.
    if (Code == 200 || Content.Num() < kDownloadChunkSize)
    {
        FinishDownloadSuccess();
        return;
    }

    // More to download — request the next chunk.
    DownloadNextChunk();
}

void UInoLiteRtLmSubsystem::FinishDownloadSuccess()
{
    CleanupDownload();

    // Rename .partial → final path.
    const FString PartialPath = PendingDownloadTargetPath + TEXT(".partial");
    if (!IFileManager::Get().Move(
            *PendingDownloadTargetPath, *PartialPath, /*Replace=*/true))
    {
        IFileManager::Get().Delete(*PartialPath);
        FinishDownloadError(FString::Printf(
            TEXT("Failed to rename %s → %s"), *PartialPath, *PendingDownloadTargetPath));
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Download: FinishDownloadSuccess — saved to %s (%lld bytes; %.1f MB)"),
           *PendingDownloadTargetPath, DownloadBytesWritten,
           DownloadBytesWritten / (1024.0 * 1024.0));

    // Terminal download-progress tick — bCompleted=true. Fires
    // exactly once per successful download, AFTER the .partial has
    // been renamed to the final path. UI handlers bound to
    // OnDownloadProgress can use this to flip from "downloading" to
    // "loading" immediately, without waiting for OnLoaded (which only
    // fires after SHA-256 verify + engine construction, seconds later).
    // On download failure this does NOT fire — the caller sees
    // OnLoaded(false, error) instead.
    PendingOnDownloadProgress.ExecuteIfBound(
        100.0f,
        DownloadBytesWritten,
        DownloadTotalBytes > 0 ? DownloadTotalBytes : DownloadBytesWritten,
        /*bCompleted=*/ true);

    // Verify the freshly-downloaded file against the entry's ExpectedSha256
    // before handing it to the native engine. bAllowRedownloadOnMismatch=false
    // so that a corrupt upstream can't put us in an infinite redownload loop —
    // if a just-downloaded file fails verification we treat it as a hard error.
    const UInoAgentsSettings*     PostSettings = UInoAgentsSettings::Get();
    const FInoLiteRtLmModelEntry* PostEntry    = PostSettings
        ? PostSettings->FindModelByFileName(LoadedConfig.ModelFileName)
        : nullptr;
    VerifyAndLoad(PendingDownloadTargetPath, PostEntry, PendingOnLoaded,
                  /*bAllowRedownloadOnMismatch=*/ false);
}

void UInoLiteRtLmSubsystem::FinishDownloadError(const FString& Error)
{
    CleanupDownload();
    IFileManager::Get().Delete(*(PendingDownloadTargetPath + TEXT(".partial")));
    bLoadInFlight = false;
    UE_LOG(LogInoAgents, Verbose,
           TEXT("LiteRtLm: Subsystem: bLoadInFlight → false (FinishDownloadError)"));
    LoadedConfig  = FInoLiteRtLmModelConfig();
    UE_LOG(LogInoAgents, Error, TEXT("LiteRtLm: Download: FinishDownloadError: %s"), *Error);
    PendingOnLoaded.ExecuteIfBound(false, Error);
}

void UInoLiteRtLmSubsystem::CleanupDownload()
{
    if (DownloadFileHandle != nullptr)
    {
        delete DownloadFileHandle;
        DownloadFileHandle = nullptr;
    }
}

void UInoLiteRtLmSubsystem::VerifyAndLoad(
    const FString&                   ModelPath,
    const FInoLiteRtLmModelEntry*    Entry,
    const FOnInoLiteRtLmModelLoaded& OnLoaded,
    bool                             bAllowRedownloadOnMismatch)
{
    check(IsInGameThread());

    // No registry entry at all, or entry has no expected hash → verification
    // is effectively opt-in per model and this one is opted out. Proceed
    // straight to load with the same behaviour the plugin had before SHA
    // checks existed.
    if (Entry == nullptr || Entry->ExpectedSha256.IsEmpty())
    {
        if (Entry != nullptr)
        {
            UE_LOG(LogInoAgents, Verbose,
                   TEXT("LiteRtLm: Subsystem: VerifyAndLoad — no ExpectedSha256 configured for '%s'; skipping verification"),
                   *Entry->ModelFileName);
        }
        ProceedWithLoad(ModelPath, OnLoaded);
        return;
    }

    // Capture everything we need by value so the ThreadPool lambda has no
    // lifetime dependency on Entry (which points into a UPROPERTY TArray
    // that could, in principle, be edited from the editor mid-verify).
    const FString                       ExpectedHash  = Entry->ExpectedSha256.ToLower();
    const FString                       RedownloadUrl = Entry->DownloadUrl;
    const FString                       ModelFileName = Entry->ModelFileName;
    const FString                       PathCopy      = ModelPath;
    TWeakObjectPtr<UInoLiteRtLmSubsystem> WeakThis(this);
    const double                         TStart        = FPlatformTime::Seconds();

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Subsystem: VerifyAndLoad computing SHA-256 of %s (expected=%s; may take several seconds for multi-GB files)"),
           *PathCopy, *ExpectedHash);

    Async(EAsyncExecution::ThreadPool,
        [PathCopy, ExpectedHash, RedownloadUrl, ModelFileName, WeakThis,
         OnLoaded, bAllowRedownloadOnMismatch, TStart]()
    {
        // ============== WORKER THREAD ==============
        const FString ActualHash = InoAgents::ComputeFileSha256(PathCopy).ToLower();
        const double  Elapsed    = FPlatformTime::Seconds() - TStart;

        AsyncTask(ENamedThreads::GameThread,
            [PathCopy, ExpectedHash, ActualHash, RedownloadUrl, ModelFileName,
             WeakThis, OnLoaded, bAllowRedownloadOnMismatch, Elapsed]()
        {
            // ============== GAME THREAD ==============
            UInoLiteRtLmSubsystem* Self = WeakThis.Get();
            if (Self == nullptr)
            {
                // Subsystem was torn down while the hash was running. The
                // user can't see a result any more, so drop silently.
                return;
            }

            if (!ActualHash.IsEmpty() && ActualHash == ExpectedHash)
            {
                UE_LOG(LogInoAgents, Log,
                       TEXT("LiteRtLm: Subsystem: VerifyAndLoad SHA-256 OK for %s (%.1f s, hash=%s)"),
                       *ModelFileName, Elapsed, *ActualHash);
                Self->ProceedWithLoad(PathCopy, OnLoaded);
                return;
            }

            // --- Mismatch / read failure handling ---

            if (ActualHash.IsEmpty())
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("LiteRtLm: Subsystem: VerifyAndLoad failed to compute SHA-256 of %s (file unreadable or gone); "
                            "treating as verification failure"),
                       *PathCopy);
            }
            else
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("LiteRtLm: Subsystem: VerifyAndLoad SHA-256 mismatch for %s. Expected %s, got %s."),
                       *ModelFileName, *ExpectedHash, *ActualHash);
            }

            // Delete the bad file so the next load attempt sees a clean slate
            // and won't waste another multi-second hash on the same bytes.
            if (!IFileManager::Get().Delete(*PathCopy, /*RequireExists=*/ false,
                                            /*EvenReadOnly=*/ true))
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("LiteRtLm: Subsystem: VerifyAndLoad also failed to delete %s — "
                            "manual cleanup may be required"),
                       *PathCopy);
            }

            if (!bAllowRedownloadOnMismatch)
            {
                // Post-download verification failure — do NOT loop.
                Self->bLoadInFlight = false;
                UE_LOG(LogInoAgents, Verbose,
                       TEXT("LiteRtLm: Subsystem: bLoadInFlight → false (post-download SHA mismatch)"));
                Self->LoadedConfig  = FInoLiteRtLmModelConfig();
                const FString Err = FString::Printf(
                    TEXT("Model '%s' failed SHA-256 verification immediately after download "
                         "(expected %s, got %s). The download may be corrupt or the configured "
                         "hash may be wrong."),
                    *ModelFileName, *ExpectedHash,
                    ActualHash.IsEmpty() ? TEXT("<unreadable>") : *ActualHash);
                UE_LOG(LogInoAgents, Error, TEXT("LiteRtLm: Subsystem: VerifyAndLoad FAILED: %s"), *Err);
                OnLoaded.ExecuteIfBound(false, Err);
                return;
            }

            // Cached file went bad — try a fresh download if we have a URL.
            if (RedownloadUrl.IsEmpty())
            {
                Self->bLoadInFlight = false;
                UE_LOG(LogInoAgents, Verbose,
                       TEXT("LiteRtLm: Subsystem: bLoadInFlight → false (cached SHA mismatch, no URL)"));
                Self->LoadedConfig  = FInoLiteRtLmModelConfig();
                const FString Err = FString::Printf(
                    TEXT("Cached model '%s' failed SHA-256 verification and no DownloadUrl is "
                         "configured — cannot recover. Expected %s, got %s."),
                    *ModelFileName, *ExpectedHash,
                    ActualHash.IsEmpty() ? TEXT("<unreadable>") : *ActualHash);
                UE_LOG(LogInoAgents, Error, TEXT("LiteRtLm: Subsystem: VerifyAndLoad FAILED: %s"), *Err);
                OnLoaded.ExecuteIfBound(false, Err);
                return;
            }

            UE_LOG(LogInoAgents, Log,
                   TEXT("LiteRtLm: Subsystem: VerifyAndLoad re-downloading %s from %s"),
                   *ModelFileName, *RedownloadUrl);
            Self->StartDownload(RedownloadUrl, PathCopy, OnLoaded);
        });
    });
}

// ======================================================================
// Chat panel (dev/debug UI)
// ======================================================================
//
// File-scope state mirrors the console-command version in
// InoLiteRtLmShowChatPanelTest.cpp. When ShowChatPanel is called
// via the subsystem (Blueprint or C++), these statics are reused so
// only one panel is ever live at a time — the subsystem path and the
// console-command path share the same teardown logic in HideChatPanel.

namespace ChatPanelState
{
    TStrongObjectPtr<UInoChatBridge> Bridge;
    TSharedPtr<SInoChatPanel>        Panel;
    TSharedPtr<SWidget>                    ViewportContent;
    TWeakObjectPtr<UGameViewportClient>    HostViewport;

#if WITH_EDITOR
    FDelegateHandle PrePIEEndedHandle;
#endif
}

static UGameViewportClient* FindGameViewportForChatPanel()
{
    if (GEngine == nullptr)
    {
        return nullptr;
    }
    for (const FWorldContext& Context : GEngine->GetWorldContexts())
    {
        if (Context.GameViewport != nullptr)
        {
            return Context.GameViewport;
        }
    }
    return GEngine->GameViewport;
}

void UInoLiteRtLmSubsystem::HideChatPanel()
{
    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Subsystem: HideChatPanel called (panel_active=%s)"),
           ChatPanelState::Panel.IsValid() ? TEXT("yes") : TEXT("no"));

#if WITH_EDITOR
    if (ChatPanelState::PrePIEEndedHandle.IsValid())
    {
        FEditorDelegates::PrePIEEnded.Remove(ChatPanelState::PrePIEEndedHandle);
        ChatPanelState::PrePIEEndedHandle.Reset();
    }
#endif

    if (ChatPanelState::Bridge.IsValid())
    {
        ChatPanelState::Bridge->Detach();
    }

    if (UGameViewportClient* VC = ChatPanelState::HostViewport.Get())
    {
        if (ChatPanelState::ViewportContent.IsValid())
        {
            VC->RemoveViewportWidgetContent(ChatPanelState::ViewportContent.ToSharedRef());
        }
    }
    ChatPanelState::HostViewport.Reset();
    ChatPanelState::ViewportContent.Reset();
    ChatPanelState::Panel.Reset();
    ChatPanelState::Bridge.Reset();

    UE_LOG(LogInoAgents, Log, TEXT("LiteRtLm: Subsystem: HideChatPanel done"));
}

void UInoLiteRtLmSubsystem::ShowChatPanel(UInoLiteRtLmConversation* InConversation)
{
    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Subsystem: ShowChatPanel called (external_conversation=%s)"),
           InConversation != nullptr ? TEXT("yes") : TEXT("no"));

    // Tear down any prior panel first.
    HideChatPanel();

    // Locate a viewport.
    UGameViewportClient* VC = FindGameViewportForChatPanel();
    if (VC == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("LiteRtLm: Subsystem: ShowChatPanel FAILED — no GameViewport; start PIE first"));
        return;
    }

    // Resolve the conversation: use the caller's if provided, otherwise
    // create one ourselves (requires a loaded model).
    UInoLiteRtLmConversation* Conv = InConversation;
    if (Conv == nullptr)
    {
        if (!IsModelLoaded())
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("LiteRtLm: Subsystem: ShowChatPanel FAILED — no conversation provided and no model loaded; "
                        "either pass a conversation or load a model first"));
            return;
        }
        Conv = CreateConversation();
        if (Conv == nullptr)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("LiteRtLm: Subsystem: ShowChatPanel FAILED — CreateConversation returned null"));
            return;
        }
    }

    // Build the bridge and panel.
    UInoChatBridge* Bridge = NewObject<UInoChatBridge>();
    ChatPanelState::Bridge = TStrongObjectPtr<UInoChatBridge>(Bridge);

    TSharedRef<SInoChatPanel> Panel = SNew(SInoChatPanel)
        .OnMessageSubmitted(FOnInoChatPanelMessageSubmitted::CreateLambda(
            [](const FString& Text)
            {
                if (ChatPanelState::Bridge.IsValid())
                {
                    ChatPanelState::Bridge->SendUserMessage(Text);
                }
            }))
        .OnCancelRequested(FOnInoChatPanelCancelRequested::CreateLambda(
            []()
            {
                if (ChatPanelState::Bridge.IsValid())
                {
                    ChatPanelState::Bridge->CancelStream();
                }
            }))
        .OnDismissed(FOnInoChatPanelDismissed::CreateLambda(
            [WeakThis = TWeakObjectPtr<UInoLiteRtLmSubsystem>(this)]()
            {
                if (UInoLiteRtLmSubsystem* Self = WeakThis.Get())
                {
                    Self->HideChatPanel();
                }
            }));
    ChatPanelState::Panel = Panel;

    // Position bottom-right with 24 px padding.
    TSharedRef<SWidget> Anchor = SNew(SBox)
        .HAlign(HAlign_Right)
        .VAlign(VAlign_Bottom)
        .Padding(FMargin(0.f, 0.f, 24.f, 24.f))
        [
            Panel
        ];
    ChatPanelState::ViewportContent = Anchor;
    ChatPanelState::HostViewport    = VC;

    VC->AddViewportWidgetContent(Anchor, /*ZOrder=*/100);

    // Hand the bridge the conversation + panel.
    Bridge->Attach(Panel, Conv);

    // Focus the input on the next Slate tick.
    Panel->FocusInput();

#if WITH_EDITOR
    ChatPanelState::PrePIEEndedHandle = FEditorDelegates::PrePIEEnded.AddLambda(
        [WeakThis = TWeakObjectPtr<UInoLiteRtLmSubsystem>(this)](const bool /*bIsSimulating*/)
        {
            if (UInoLiteRtLmSubsystem* Self = WeakThis.Get())
            {
                Self->HideChatPanel();
            }
        });
#endif

    UE_LOG(LogInoAgents, Log,
           TEXT("LiteRtLm: Subsystem: ShowChatPanel ready (conversation=%s, external=%s)"),
           *Conv->GetName(),
           InConversation != nullptr ? TEXT("yes") : TEXT("no"));
}
