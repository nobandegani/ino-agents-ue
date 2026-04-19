// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/InoLiteRtLmSubsystem.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/InoLiteRtLmConversation.h"
#include "InoAgentsSettings.h"
#include "LiteRtLm/InoLiteRtLmToolBase.h"
#include "LiteRtLm/InoLiteRtLmTypes.h"
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

    UE_LOG(LogInoAgents, Log, TEXT("UInoLiteRtLmSubsystem: Initialize"));
}

void UInoLiteRtLmSubsystem::Deinitialize()
{
    UE_LOG(LogInoAgents, Log, TEXT("UInoLiteRtLmSubsystem: Deinitialize"));

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
    const FInoLiteRtLmModelConfig& Config,
    const FOnInoLiteRtLmModelLoaded& OnLoaded)
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
               TEXT("LoadModelAsync: resolved '%s' via display-name match → file name '%s'"),
               *ResolvedConfig.ModelFileName, *Entry->ModelFileName);
        ResolvedConfig.ModelFileName = Entry->ModelFileName;
    }

    // Resolve the model file path. Checks PersistentDownloadDir first
    // (downloaded/cached), then the plugin's Models/ dir (legacy dev).
    const FString ModelPath = LiteRtLmResolveModelPath(ResolvedConfig.ModelFileName);

    if (!ModelPath.IsEmpty())
    {
        // Found on disk — proceed to load.
        bLoadInFlight = true;
        LoadedConfig  = ResolvedConfig;
        ProceedWithLoad(ModelPath, OnLoaded);
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
        UE_LOG(LogInoAgents, Error, TEXT("LoadModelAsync: %s"), *Err);
        OnLoaded.ExecuteIfBound(false, Err);
        return;
    }

    // Download, then load.
    bLoadInFlight = true;
    LoadedConfig  = ResolvedConfig;

    const FString TargetDir = FPaths::Combine(
        FPaths::ProjectPersistentDownloadDir(),
        TEXT("InoAgents"), TEXT("Models"));
    IFileManager::Get().MakeDirectory(*TargetDir, /*Tree=*/true);

    const FString TargetPath = FPaths::Combine(TargetDir, ResolvedConfig.ModelFileName);

    UE_LOG(LogInoAgents, Log,
           TEXT("LoadModelAsync: model not found locally, downloading from %s"),
           *Entry->DownloadUrl);

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
           TEXT("LoadModelAsync: dispatching async load of %s (backend=%s, activation=%d)"),
           *ModelPathCopy, ANSI_TO_TCHAR(LiteRtLmBackendToString(BackendCopy)),
           static_cast<int32>(ActivationType));

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

            UInoLiteRtLmSubsystem* Subsys = WeakThis.Get();
            Subsys->bLoadInFlight = false;

            if (NewEngine == nullptr)
            {
                // Load failed. Clear the config reference so a subsequent
                // retry can succeed.
                Subsys->LoadedConfig = FInoLiteRtLmModelConfig();
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

bool UInoLiteRtLmSubsystem::IsModelLoaded() const
{
    return Engine != nullptr;
}

void UInoLiteRtLmSubsystem::UnloadModel()
{
    check(IsInGameThread());

    // Tear down the active conversation (if any) BEFORE the engine. LiteRT-LM
    // sessions hold raw pointers into the engine's LlmExecutor; deleting the
    // engine first and the conversation later causes ~SessionBasic to AV when
    // GC eventually runs BeginDestroy on the conversation UObject.
    if (UInoLiteRtLmConversation* Conv = ActiveConversation.Get())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("UnloadModel: shutting down active conversation before engine teardown"));
        Conv->Shutdown();
    }
    ActiveConversation.Reset();

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
    LoadedConfig = FInoLiteRtLmModelConfig();

    UE_LOG(LogInoAgents, Log, TEXT("UInoLiteRtLmSubsystem: UnloadModel complete"));
}

UInoLiteRtLmConversation* UInoLiteRtLmSubsystem::CreateConversation()
{
    return CreateConversationWithHistory(TArray<FInoLiteRtLmMessage>());
}

UInoLiteRtLmConversation* UInoLiteRtLmSubsystem::CreateConversationWithHistory(
    const TArray<FInoLiteRtLmMessage>& InitialMessages)
{
    check(IsInGameThread());

    if (Engine == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("CreateConversation: no model loaded — call LoadModelAsync first"));
        return nullptr;
    }

    if (LoadedConfig.ModelFileName.IsEmpty())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("CreateConversation: LoadedConfig is null despite Engine being "
                    "set — this should be impossible"));
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
               TEXT("CreateConversation: a prior conversation is still active; "
                    "shutting it down to honour the single-conversation-per-engine "
                    "invariant. Callers holding a reference to it will see "
                    "SendMessageAsync error out."));
        Prior->Shutdown();
    }
    ActiveConversation.Reset();

    UInoLiteRtLmConversation* Conv = NewObject<UInoLiteRtLmConversation>();
    Conv->Initialize(this, Engine, LoadedConfig, InitialMessages);
    ActiveConversation = Conv;
    return Conv;
}

// ----------------------------------------------------------------------
// Tool registry (D.4)
// ----------------------------------------------------------------------

void UInoLiteRtLmSubsystem::RegisterTool(UInoLiteRtLmToolBase* Tool)
{
    check(IsInGameThread());

    if (Tool == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("RegisterTool: the supplied Tool is null"));
        return;
    }

    const FName DeclaredName = Tool->ToolName;
    if (DeclaredName == NAME_None)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("RegisterTool: tool %s has an empty ToolName — "
                    "every tool must have a unique non-empty name"),
               *Tool->GetName());
        return;
    }

    // Validate the schema built from the tool's properties. It must
    // parse as JSON and its function.name must match ToolName.
    const FString SchemaJson = Tool->BuildSchemaJson();
    if (SchemaJson.IsEmpty())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("RegisterTool: tool %s produced an empty schema from BuildSchemaJson"),
               *DeclaredName.ToString());
        return;
    }

    TSharedPtr<FJsonObject> SchemaObj;
    const TSharedRef<TJsonReader<>> SchemaReader = TJsonReaderFactory<>::Create(SchemaJson);
    if (!FJsonSerializer::Deserialize(SchemaReader, SchemaObj) || !SchemaObj.IsValid())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("RegisterTool: tool %s schema does not parse as JSON. Schema was: %s"),
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
                       TEXT("RegisterTool: tool %s schema name mismatch: "
                            "ToolName==%s but schema function.name==%s. Refusing to register."),
                       *Tool->GetName(), *DeclaredName.ToString(), *SchemaName);
                return;
            }
        }
    }

    if (Tools.Contains(DeclaredName))
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("RegisterTool: replacing existing registration for tool \"%s\""),
               *DeclaredName.ToString());
    }

    Tools.Add(DeclaredName, Tool);

    UE_LOG(LogInoAgents, Log,
           TEXT("RegisterTool: registered tool \"%s\""),
           *DeclaredName.ToString());
}

void UInoLiteRtLmSubsystem::UnregisterTool(FName ToolName)
{
    check(IsInGameThread());

    const int32 NumRemoved = Tools.Remove(ToolName);
    if (NumRemoved == 0)
    {
        UE_LOG(LogInoAgents, Verbose,
               TEXT("UnregisterTool: no tool registered under name \"%s\" — no-op"),
               *ToolName.ToString());
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UnregisterTool: removed tool \"%s\""),
           *ToolName.ToString());
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
                   TEXT("BuildToolsJsonForConversation: tool \"%s\" schema does not parse; dropping"),
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
           TEXT("LoadModelAsync: starting chunked download (%lld-byte chunks)"),
           kDownloadChunkSize);

    DownloadNextChunk();
}

void UInoLiteRtLmSubsystem::DownloadNextChunk()
{
    const int64 RangeStart = DownloadBytesWritten;
    const int64 RangeEnd   = RangeStart + kDownloadChunkSize - 1;

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

    // Broadcast progress.
    OnDownloadProgress.Broadcast(
        0.0f,
        DownloadBytesWritten,
        -1);

    UE_LOG(LogInoAgents, Log,
           TEXT("LoadModelAsync: downloaded %lld MB so far"),
           DownloadBytesWritten / (1024 * 1024));

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
           TEXT("LoadModelAsync: model downloaded and saved to %s (%lld bytes)"),
           *PendingDownloadTargetPath, DownloadBytesWritten);

    ProceedWithLoad(PendingDownloadTargetPath, PendingOnLoaded);
}

void UInoLiteRtLmSubsystem::FinishDownloadError(const FString& Error)
{
    CleanupDownload();
    IFileManager::Get().Delete(*(PendingDownloadTargetPath + TEXT(".partial")));
    bLoadInFlight = false;
    LoadedConfig  = FInoLiteRtLmModelConfig();
    UE_LOG(LogInoAgents, Error, TEXT("LoadModelAsync: %s"), *Error);
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

    UE_LOG(LogInoAgents, Log, TEXT("UInoLiteRtLmSubsystem::HideChatPanel: done"));
}

void UInoLiteRtLmSubsystem::ShowChatPanel(UInoLiteRtLmConversation* InConversation)
{
    // Tear down any prior panel first.
    HideChatPanel();

    // Locate a viewport.
    UGameViewportClient* VC = FindGameViewportForChatPanel();
    if (VC == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("ShowChatPanel: no GameViewport — start PIE first."));
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
                   TEXT("ShowChatPanel: no conversation provided and no model loaded — "
                        "either pass a conversation or load a model first."));
            return;
        }
        Conv = CreateConversation();
        if (Conv == nullptr)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("ShowChatPanel: CreateConversation returned null."));
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
           TEXT("ShowChatPanel: ready (conversation=%s, external=%s)"),
           *Conv->GetName(),
           InConversation != nullptr ? TEXT("yes") : TEXT("no"));
}
