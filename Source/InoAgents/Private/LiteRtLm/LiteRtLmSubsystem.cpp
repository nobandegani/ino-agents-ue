// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmSubsystem.h"

#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmConversation.h"
#include "InoAgentsSettings.h"
#include "LiteRtLm/LiteRtLmTool.h"
#include "LiteRtLm/LiteRtLmTypes.h"
#include "UI/Slate/InoAgentsChatBridge.h"
#include "UI/Slate/SInoAgentsChatPanel.h"

#include "Async/Async.h"
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

void ULiteRtLmSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    // Zero-init members explicitly in case a fresh subsystem is ever
    // reused (hot reload in editor). Nothing expensive happens here —
    // engine loading is lazy via LoadModelAsync.
    Engine       = nullptr;
    Settings     = nullptr;
    LoadedConfig = FLiteRtLmModelConfig();
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

void ULiteRtLmSubsystem::LoadModelAsync(
    const FLiteRtLmModelConfig& Config,
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

    // Resolve the model file path. Checks PersistentDownloadDir first
    // (downloaded/cached), then the plugin's Models/ dir (legacy dev).
    const FString ModelPath = LiteRtLmResolveModelPath(Config.ModelFileName);

    if (!ModelPath.IsEmpty())
    {
        // Found on disk — proceed to load.
        bLoadInFlight = true;
        LoadedConfig  = Config;
        ProceedWithLoad(ModelPath, OnLoaded);
        return;
    }

    // Not on disk — look up the download URL in settings.
    const UInoAgentsSettings* AgentSettings = UInoAgentsSettings::Get();
    const FLiteRtLmModelEntry* Entry = AgentSettings
        ? AgentSettings->FindModelByFileName(Config.ModelFileName)
        : nullptr;

    if (Entry == nullptr || Entry->DownloadUrl.IsEmpty())
    {
        const FString Err = FString::Printf(
            TEXT("Model '%s' not found on disk and no download URL configured. "
                 "Add an entry in Project Settings → Plugins → InoAgents → "
                 "LiteRT-LM → Models."),
            *Config.ModelFileName);
        UE_LOG(LogInoAgents, Error, TEXT("LoadModelAsync: %s"), *Err);
        OnLoaded.ExecuteIfBound(false, Err);
        return;
    }

    // Download, then load.
    bLoadInFlight = true;
    LoadedConfig  = Config;

    const FString TargetDir = FPaths::Combine(
        FPaths::ProjectPersistentDownloadDir(),
        TEXT("InoAgents"), TEXT("Models"));
    IFileManager::Get().MakeDirectory(*TargetDir, /*Tree=*/true);

    const FString TargetPath = FPaths::Combine(TargetDir, Config.ModelFileName);

    UE_LOG(LogInoAgents, Log,
           TEXT("LoadModelAsync: model not found locally, downloading from %s"),
           *Entry->DownloadUrl);

    StartDownload(Entry->DownloadUrl, TargetPath, OnLoaded);
}

void ULiteRtLmSubsystem::ProceedWithLoad(
    const FString& ModelPath, const FOnLiteRtLmModelLoaded& OnLoaded)
{
    TWeakObjectPtr<ULiteRtLmSubsystem> WeakThis(this);
    const FString                     ModelPathCopy = ModelPath;
    const ELiteRtLmBackend            BackendCopy   = LoadedConfig.Backend;
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
                Subsys->LoadedConfig = FLiteRtLmModelConfig();
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

    // Tear down the active conversation (if any) BEFORE the engine. LiteRT-LM
    // sessions hold raw pointers into the engine's LlmExecutor; deleting the
    // engine first and the conversation later causes ~SessionBasic to AV when
    // GC eventually runs BeginDestroy on the conversation UObject.
    if (ULiteRtLmConversation* Conv = ActiveConversation.Get())
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
    LoadedConfig = FLiteRtLmModelConfig();

    UE_LOG(LogInoAgents, Log, TEXT("ULiteRtLmSubsystem: UnloadModel complete"));
}

ULiteRtLmConversation* ULiteRtLmSubsystem::CreateConversation()
{
    check(IsInGameThread());

    if (Engine == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("CreateConversation: no model loaded — call LoadModelAsync first"));
        return nullptr;
    }

    // LoadedConfig is guaranteed non-null whenever Engine is non-null
    // (see the success path of LoadModelAsync's completion lambda), but
    // defensively check anyway.
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
    if (ULiteRtLmConversation* Prior = ActiveConversation.Get())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("CreateConversation: a prior conversation is still active; "
                    "shutting it down to honour the single-conversation-per-engine "
                    "invariant. Callers holding a reference to it will see "
                    "SendMessageAsync error out."));
        Prior->Shutdown();
    }
    ActiveConversation.Reset();

    ULiteRtLmConversation* Conv = NewObject<ULiteRtLmConversation>();
    Conv->Initialize(this, Engine, LoadedConfig);
    ActiveConversation = Conv;
    return Conv;
}

// ----------------------------------------------------------------------
// Tool registry (D.4)
// ----------------------------------------------------------------------

void ULiteRtLmSubsystem::RegisterTool(TScriptInterface<ILiteRtLmTool> Tool)
{
    check(IsInGameThread());

    // Defensive null check — TScriptInterface can carry a UObject with
    // a null interface pointer if the UObject doesn't actually implement
    // the interface. The second check catches that case.
    UObject* const ToolObj = Tool.GetObject();
    if (ToolObj == nullptr || Tool.GetInterface() == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("RegisterTool: the supplied Tool is null or does not implement ILiteRtLmTool"));
        return;
    }

    // Look up the tool's declared name via the Execute_* wrapper.
    // ILiteRtLmTool is a BlueprintNativeEvent interface so we MUST
    // go through the wrapper (ILiteRtLmTool::Execute_GetToolName),
    // not the raw _Implementation — the wrapper handles both C++
    // and Blueprint implementors.
    const FName DeclaredName = ILiteRtLmTool::Execute_GetToolName(ToolObj);
    if (DeclaredName == NAME_None)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("RegisterTool: tool %s returned NAME_None from GetToolName — "
                    "every tool must have a unique non-empty name"),
               *ToolObj->GetName());
        return;
    }

    // Validate the schema: it must parse, and its function.name must
    // match DeclaredName. Catching a mismatch here prevents the
    // confusing downstream case where LiteRT-LM advertises a tool
    // with name X to the model but our registry routes the resulting
    // call to name Y.
    const FString SchemaJson = ILiteRtLmTool::Execute_GetToolSchemaJson(ToolObj);
    if (SchemaJson.IsEmpty())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("RegisterTool: tool %s returned an empty schema from GetToolSchemaJson"),
               *DeclaredName.ToString());
        return;
    }

    TSharedPtr<FJsonObject> SchemaObj;
    const TSharedRef<TJsonReader<>> SchemaReader = TJsonReaderFactory<>::Create(SchemaJson);
    if (!FJsonSerializer::Deserialize(SchemaReader, SchemaObj) || !SchemaObj.IsValid())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("RegisterTool: tool %s has a schema that does not parse as JSON. Schema was: %s"),
               *DeclaredName.ToString(), *SchemaJson);
        return;
    }

    const TSharedPtr<FJsonObject>* FunctionObjPtr = nullptr;
    if (!SchemaObj->TryGetObjectField(TEXT("function"), FunctionObjPtr)
        || FunctionObjPtr == nullptr
        || !FunctionObjPtr->IsValid())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("RegisterTool: tool %s schema is missing the top-level \"function\" object"),
               *DeclaredName.ToString());
        return;
    }

    FString SchemaName;
    if (!(*FunctionObjPtr)->TryGetStringField(TEXT("name"), SchemaName))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("RegisterTool: tool %s schema is missing function.name"),
               *DeclaredName.ToString());
        return;
    }

    if (FName(*SchemaName) != DeclaredName)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("RegisterTool: tool %s has a schema name mismatch: "
                    "GetToolName()==%s but schema function.name==%s. Refusing to register."),
               *ToolObj->GetName(), *DeclaredName.ToString(), *SchemaName);
        return;
    }

    // Warn on replacement so a developer notices double-registration
    // bugs, but allow it — sometimes reregistration is intentional
    // (e.g. Blueprint reload in the editor).
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

void ULiteRtLmSubsystem::UnregisterTool(FName ToolName)
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

TScriptInterface<ILiteRtLmTool> ULiteRtLmSubsystem::FindTool(FName ToolName) const
{
    if (const TScriptInterface<ILiteRtLmTool>* Found = Tools.Find(ToolName))
    {
        return *Found;
    }
    return TScriptInterface<ILiteRtLmTool>();
}

FString ULiteRtLmSubsystem::BuildToolsJsonForConversation() const
{
    check(IsInGameThread());

    if (Tools.Num() == 0)
    {
        // Empty string is the signal to CreateConversation / Initialize
        // that tools_json should be left NULL in the native config.
        return FString();
    }

    // Build a JSON array of schema objects. Each schema was validated
    // as parseable JSON at RegisterTool time, so here we just
    // re-deserialise and stitch the objects into an array. We could
    // in theory string-concat the raw schema JSON with commas between,
    // but that would require extra care around trailing whitespace /
    // comments inside schema strings; going through FJsonObject keeps
    // the output canonical.
    TArray<TSharedPtr<FJsonValue>> SchemaArray;
    SchemaArray.Reserve(Tools.Num());

    for (const TPair<FName, TScriptInterface<ILiteRtLmTool>>& Pair : Tools)
    {
        UObject* const ToolObj = Pair.Value.GetObject();
        if (ToolObj == nullptr)
        {
            // Shouldn't happen — RegisterTool rejects nulls — but
            // handle defensively in case something cleared the
            // UObject behind our back.
            continue;
        }

        const FString SchemaJson = ILiteRtLmTool::Execute_GetToolSchemaJson(ToolObj);

        TSharedPtr<FJsonObject> SchemaObj;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SchemaJson);
        if (!FJsonSerializer::Deserialize(Reader, SchemaObj) || !SchemaObj.IsValid())
        {
            // Schema was valid at registration time; if it is no
            // longer valid, the implementor changed it behind our
            // back (e.g. a Blueprint tool whose schema is dynamic).
            // Log and drop it.
            UE_LOG(LogInoAgents, Warning,
                   TEXT("BuildToolsJsonForConversation: tool \"%s\" schema no longer parses; dropping"),
                   *Pair.Key.ToString());
            continue;
        }

        SchemaArray.Add(MakeShared<FJsonValueObject>(SchemaObj));
    }

    if (SchemaArray.Num() == 0)
    {
        return FString();
    }

    // Condensed writer keeps the output a single compact line —
    // smaller payload to pass through the C API and cleaner to log.
    FString OutJson;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
    FJsonSerializer::Serialize(SchemaArray, Writer);

    return OutJson;
}

// ======================================================================
// Model download
// ======================================================================

void ULiteRtLmSubsystem::StartDownload(
    const FString& Url, const FString& TargetPath,
    const FOnLiteRtLmModelLoaded& OnLoaded)
{
    PendingDownloadTargetPath = TargetPath;
    PendingOnLoaded           = OnLoaded;

    DownloadRequest = FHttpModule::Get().CreateRequest();
    DownloadRequest->SetURL(Url);
    DownloadRequest->SetVerb(TEXT("GET"));
    DownloadRequest->SetHeader(TEXT("Accept"), TEXT("*/*"));

    DownloadRequest->OnRequestProgress64().BindUObject(
        this, &ULiteRtLmSubsystem::HandleDownloadProgress);
    DownloadRequest->OnProcessRequestComplete().BindUObject(
        this, &ULiteRtLmSubsystem::HandleDownloadComplete);

    DownloadRequest->ProcessRequest();
}

void ULiteRtLmSubsystem::HandleDownloadProgress(
    FHttpRequestPtr /*Request*/, uint64 /*BytesSent*/, uint64 BytesReceived)
{
    int64 TotalBytes = -1;
    if (DownloadRequest.IsValid())
    {
        if (const FHttpResponsePtr Resp = DownloadRequest->GetResponse())
        {
            const FString ContentLength = Resp->GetHeader(TEXT("Content-Length"));
            if (!ContentLength.IsEmpty())
            {
                TotalBytes = FCString::Atoi64(*ContentLength);
            }
        }
    }

    const float Percent = (TotalBytes > 0)
        ? (static_cast<float>(BytesReceived) / static_cast<float>(TotalBytes)) * 100.0f
        : 0.0f;

    OnDownloadProgress.Broadcast(Percent, static_cast<int64>(BytesReceived), TotalBytes);
}

void ULiteRtLmSubsystem::HandleDownloadComplete(
    FHttpRequestPtr /*Request*/, FHttpResponsePtr Response, bool bSucceeded)
{
    DownloadRequest.Reset();

    if (!bSucceeded || !Response.IsValid())
    {
        bLoadInFlight = false;
        LoadedConfig  = FLiteRtLmModelConfig();
        const FString Err = TEXT("Model download failed (network error)");
        UE_LOG(LogInoAgents, Error, TEXT("LoadModelAsync: %s"), *Err);
        PendingOnLoaded.ExecuteIfBound(false, Err);
        return;
    }

    const int32 Code = Response->GetResponseCode();
    if (Code < 200 || Code >= 300)
    {
        bLoadInFlight = false;
        LoadedConfig  = FLiteRtLmModelConfig();
        const FString Err = FString::Printf(TEXT("Model download failed: HTTP %d"), Code);
        UE_LOG(LogInoAgents, Error, TEXT("LoadModelAsync: %s"), *Err);
        PendingOnLoaded.ExecuteIfBound(false, Err);
        return;
    }

    const TArray<uint8>& Content = Response->GetContent();
    if (!FFileHelper::SaveArrayToFile(Content, *PendingDownloadTargetPath))
    {
        bLoadInFlight = false;
        LoadedConfig  = FLiteRtLmModelConfig();
        const FString Err = FString::Printf(
            TEXT("Failed to save model to %s"), *PendingDownloadTargetPath);
        UE_LOG(LogInoAgents, Error, TEXT("LoadModelAsync: %s"), *Err);
        PendingOnLoaded.ExecuteIfBound(false, Err);
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("LoadModelAsync: model downloaded and saved to %s (%d bytes)"),
           *PendingDownloadTargetPath, Content.Num());

    // Now load normally.
    ProceedWithLoad(PendingDownloadTargetPath, PendingOnLoaded);
}

// ======================================================================
// Chat panel (dev/debug UI)
// ======================================================================
//
// File-scope state mirrors the console-command version in
// InoAgentsLiteRtLmShowChatPanelTest.cpp. When ShowChatPanel is called
// via the subsystem (Blueprint or C++), these statics are reused so
// only one panel is ever live at a time — the subsystem path and the
// console-command path share the same teardown logic in HideChatPanel.

namespace ChatPanelState
{
    TStrongObjectPtr<UInoAgentsChatBridge> Bridge;
    TSharedPtr<SInoAgentsChatPanel>        Panel;
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

void ULiteRtLmSubsystem::HideChatPanel()
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

    UE_LOG(LogInoAgents, Log, TEXT("ULiteRtLmSubsystem::HideChatPanel: done"));
}

void ULiteRtLmSubsystem::ShowChatPanel(ULiteRtLmConversation* InConversation)
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
    ULiteRtLmConversation* Conv = InConversation;
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
    UInoAgentsChatBridge* Bridge = NewObject<UInoAgentsChatBridge>();
    ChatPanelState::Bridge = TStrongObjectPtr<UInoAgentsChatBridge>(Bridge);

    TSharedRef<SInoAgentsChatPanel> Panel = SNew(SInoAgentsChatPanel)
        .OnMessageSubmitted(FOnInoAgentsChatPanelMessageSubmitted::CreateLambda(
            [](const FString& Text)
            {
                if (ChatPanelState::Bridge.IsValid())
                {
                    ChatPanelState::Bridge->SendUserMessage(Text);
                }
            }))
        .OnCancelRequested(FOnInoAgentsChatPanelCancelRequested::CreateLambda(
            []()
            {
                if (ChatPanelState::Bridge.IsValid())
                {
                    ChatPanelState::Bridge->CancelStream();
                }
            }))
        .OnDismissed(FOnInoAgentsChatPanelDismissed::CreateLambda(
            [WeakThis = TWeakObjectPtr<ULiteRtLmSubsystem>(this)]()
            {
                if (ULiteRtLmSubsystem* Self = WeakThis.Get())
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
        [WeakThis = TWeakObjectPtr<ULiteRtLmSubsystem>(this)](const bool /*bIsSimulating*/)
        {
            if (ULiteRtLmSubsystem* Self = WeakThis.Get())
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
