// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmAgentComponent.h"

#include "Audio/InoAgentsLiteRtLmDialogueQueue.h"
#include "Audio/InoAgentsStreamingAudioComponent.h"
#include "InoAgentsLog.h"
#include "InoAgentsSettings.h"
#include "LiteRtLm/LiteRtLmConversation.h"
#include "LiteRtLm/LiteRtLmSubsystem.h"

#include "Engine/GameInstance.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

UInoAgentsLiteRtLmAgentComponent::UInoAgentsLiteRtLmAgentComponent(
    const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PrimaryComponentTick.bCanEverTick          = false;
    PrimaryComponentTick.bStartWithTickEnabled = false;

    // Create the audio component as a child so it shows up in the
    // details panel and inherits our scene transform for 3D
    // spatialization. All UAudioComponent properties (volume, pitch,
    // attenuation, source effect chain, etc.) are editable directly
    // on this child without any code.
    AudioComp = ObjectInitializer.CreateDefaultSubobject<UInoAgentsStreamingAudioComponent>(
        this, TEXT("StreamingAudio"));
    if (AudioComp != nullptr)
    {
        AudioComp->SetupAttachment(this);
    }
}

// ======================================================================
// Lifecycle
// ======================================================================

void UInoAgentsLiteRtLmAgentComponent::BeginPlay()
{
    Super::BeginPlay();

    if (bAutoLoadOnBeginPlay)
    {
        LoadModel();
    }
}

void UInoAgentsLiteRtLmAgentComponent::EndPlay(EEndPlayReason::Type Reason)
{
    // Tear down in reverse order: queue → conversation → subsystem ref.
    if (DialogueQueue != nullptr)
    {
        DialogueQueue->Clear();
        DialogueQueue = nullptr;
    }

    if (Conversation != nullptr)
    {
        Conversation->Shutdown();
        Conversation = nullptr;
    }

    SubsystemWeak.Reset();

    Super::EndPlay(Reason);
}

// ======================================================================
// Public API
// ======================================================================

void UInoAgentsLiteRtLmAgentComponent::LoadModel()
{
    if (ModelConfig.ModelFileName.IsEmpty())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsLiteRtLmAgentComponent::LoadModel: ModelConfig.ModelFileName "
                    "is empty — set it in the details panel."));
        OnError.Broadcast(TEXT("ModelConfig.ModelFileName is empty"));
        return;
    }

    // Find the subsystem.
    const UGameInstance* GI = GetOwner() != nullptr
        ? GetOwner()->GetGameInstance()
        : nullptr;
    ULiteRtLmSubsystem* Subsys = GI != nullptr
        ? GI->GetSubsystem<ULiteRtLmSubsystem>()
        : nullptr;

    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsLiteRtLmAgentComponent::LoadModel: no ULiteRtLmSubsystem — "
                    "start PIE or a packaged game first."));
        OnError.Broadcast(TEXT("No ULiteRtLmSubsystem"));
        return;
    }
    SubsystemWeak = Subsys;

    // If the model is already loaded (e.g. another component or smoke
    // test loaded it), skip straight to conversation creation.
    if (Subsys->IsModelLoaded())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("UInoAgentsLiteRtLmAgentComponent: model already loaded, "
                    "creating conversation directly"));
        HandleModelLoaded(true, FString());
        return;
    }

    // Check if the model file exists locally (PersistentDownloadDir or
    // plugin Models/ dir).
    const FString ModelPath = LiteRtLmResolveModelPath(ModelConfig.ModelFileName);

    if (!ModelPath.IsEmpty())
    {
        // Model found on disk — load directly.
        UE_LOG(LogInoAgents, Log,
               TEXT("UInoAgentsLiteRtLmAgentComponent: loading model from %s"),
               *ModelPath);

        FOnLiteRtLmModelLoaded OnLoaded;
        OnLoaded.BindDynamic(this, &UInoAgentsLiteRtLmAgentComponent::HandleModelLoaded);
        Subsys->LoadModelAsync(ModelConfig, OnLoaded);
        return;
    }

    // Model not on disk — look up the download URL in settings.
    const UInoAgentsSettings* Settings = UInoAgentsSettings::Get();
    const FLiteRtLmModelEntry* Entry = Settings != nullptr
        ? Settings->FindModelByFileName(ModelConfig.ModelFileName)
        : nullptr;

    if (Entry == nullptr || Entry->DownloadUrl.IsEmpty())
    {
        const FString Err = FString::Printf(
            TEXT("Model '%s' not found on disk and no download URL configured. "
                 "Add an entry in Project Settings → Plugins → InoAgents LiteRT-LM → Models."),
            *ModelConfig.ModelFileName);
        UE_LOG(LogInoAgents, Error, TEXT("UInoAgentsLiteRtLmAgentComponent: %s"), *Err);
        OnError.Broadcast(Err);
        return;
    }

    // Download to PersistentDownloadDir.
    const FString TargetDir = FPaths::Combine(
        FPaths::ProjectPersistentDownloadDir(),
        TEXT("InoAgents"), TEXT("Models"));
    IFileManager::Get().MakeDirectory(*TargetDir, /*Tree=*/true);

    const FString TargetPath = FPaths::Combine(TargetDir, ModelConfig.ModelFileName);

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsLiteRtLmAgentComponent: model not found locally, "
                "downloading from %s → %s"),
           *Entry->DownloadUrl, *TargetPath);

    DownloadModel(Entry->DownloadUrl, TargetPath);
}

void UInoAgentsLiteRtLmAgentComponent::SendMessage(const FString& Text)
{
    if (Conversation == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsLiteRtLmAgentComponent::SendMessage: no conversation — "
                    "model not loaded yet or LoadModel failed."));
        OnError.Broadcast(TEXT("No conversation — model not loaded yet"));
        return;
    }

    Conversation->SendMessageAsync(Text);
}

void UInoAgentsLiteRtLmAgentComponent::Cancel()
{
    if (Conversation != nullptr)
    {
        Conversation->Cancel();
    }
}

void UInoAgentsLiteRtLmAgentComponent::ShowChatPanel()
{
    if (ULiteRtLmSubsystem* Subsys = SubsystemWeak.Get())
    {
        Subsys->ShowChatPanel(Conversation);
    }
}

void UInoAgentsLiteRtLmAgentComponent::HideChatPanel()
{
    if (ULiteRtLmSubsystem* Subsys = SubsystemWeak.Get())
    {
        Subsys->HideChatPanel();
    }
}

bool UInoAgentsLiteRtLmAgentComponent::IsModelLoaded() const
{
    if (const ULiteRtLmSubsystem* Subsys = SubsystemWeak.Get())
    {
        return Subsys->IsModelLoaded();
    }
    return false;
}

bool UInoAgentsLiteRtLmAgentComponent::IsStreaming() const
{
    if (Conversation != nullptr)
    {
        return Conversation->IsStreamingInFlight();
    }
    return false;
}

// ======================================================================
// Delegate trampolines
// ======================================================================

void UInoAgentsLiteRtLmAgentComponent::HandleModelLoaded(
    bool bSuccess, FString ErrorMessage)
{
    if (!bSuccess)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsLiteRtLmAgentComponent: model load FAILED: %s"),
               *ErrorMessage);
        OnModelLoaded.Broadcast(false, ErrorMessage);
        OnError.Broadcast(FString::Printf(TEXT("Model load failed: %s"), *ErrorMessage));
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsLiteRtLmAgentComponent: model loaded, creating conversation"));

    CreateConversationAndQueue();

    OnModelLoaded.Broadcast(true, FString());
}

void UInoAgentsLiteRtLmAgentComponent::HandleToken(FString Chunk)
{
    OnToken.Broadcast(Chunk);
}

void UInoAgentsLiteRtLmAgentComponent::HandleSentence(FString RawText, FString CleanText)
{
    OnSentence.Broadcast(RawText, CleanText);
}

void UInoAgentsLiteRtLmAgentComponent::HandleComplete(FString FullText)
{
    OnComplete.Broadcast(FullText);
}

void UInoAgentsLiteRtLmAgentComponent::HandleError(FString ErrorMessage)
{
    OnError.Broadcast(ErrorMessage);
}

void UInoAgentsLiteRtLmAgentComponent::HandleAudioFinished()
{
    OnAudioFinished.Broadcast();
}

// ======================================================================
// Internal
// ======================================================================

void UInoAgentsLiteRtLmAgentComponent::CreateConversationAndQueue()
{
    ULiteRtLmSubsystem* Subsys = SubsystemWeak.Get();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsLiteRtLmAgentComponent: subsystem gone during "
                    "CreateConversationAndQueue"));
        return;
    }

    // Create the conversation.
    Conversation = Subsys->CreateConversation();
    if (Conversation == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsLiteRtLmAgentComponent: CreateConversation returned null"));
        OnError.Broadcast(TEXT("CreateConversation returned null"));
        return;
    }

    // Bind conversation delegates → trampolines → component delegates.
    Conversation->OnToken.AddDynamic(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleToken);
    Conversation->OnSentence.AddDynamic(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleSentence);
    Conversation->OnComplete.AddDynamic(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleComplete);
    Conversation->OnError.AddDynamic(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleError);

    // Create and initialize the dialogue queue. It auto-binds to
    // the conversation's OnSentence + OnNewLine for TTS dispatch.
    DialogueQueue = NewObject<UInoAgentsLiteRtLmDialogueQueue>(this);
    DialogueQueue->Initialize(
        this,              // WorldContextObject
        AudioComp,         // audio component
        Conversation,      // conversation to listen to
        VoiceId,           // default voice
        TtsRequestTemplate,// ElevenLabs settings
        PauseDurationMs);  // pause between lines

    DialogueQueue->OnAllComplete.AddDynamic(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleAudioFinished);

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsLiteRtLmAgentComponent: ready (conversation=%s, voice=%s)"),
           *Conversation->GetName(), *VoiceId);
}

// ======================================================================
// Model download
// ======================================================================

void UInoAgentsLiteRtLmAgentComponent::DownloadModel(
    const FString& Url, const FString& TargetPath)
{
    PendingDownloadTargetPath = TargetPath;

    DownloadRequest = FHttpModule::Get().CreateRequest();
    DownloadRequest->SetURL(Url);
    DownloadRequest->SetVerb(TEXT("GET"));
    DownloadRequest->SetHeader(TEXT("Accept"), TEXT("*/*"));

    DownloadRequest->OnRequestProgress64().BindUObject(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleDownloadProgress);
    DownloadRequest->OnProcessRequestComplete().BindUObject(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleDownloadComplete);

    DownloadRequest->ProcessRequest();
}

void UInoAgentsLiteRtLmAgentComponent::HandleDownloadProgress(
    FHttpRequestPtr /*Request*/, uint64 /*BytesSent*/, uint64 BytesReceived)
{
    // Content-Length may not always be available — derive total from
    // the response header if we can, otherwise report -1.
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

void UInoAgentsLiteRtLmAgentComponent::HandleDownloadComplete(
    FHttpRequestPtr /*Request*/, FHttpResponsePtr Response, bool bSucceeded)
{
    DownloadRequest.Reset();

    if (!bSucceeded || !Response.IsValid())
    {
        const FString Err = TEXT("Model download failed (network error)");
        UE_LOG(LogInoAgents, Error, TEXT("UInoAgentsLiteRtLmAgentComponent: %s"), *Err);
        OnError.Broadcast(Err);
        return;
    }

    const int32 Code = Response->GetResponseCode();
    if (Code < 200 || Code >= 300)
    {
        const FString Err = FString::Printf(
            TEXT("Model download failed: HTTP %d"), Code);
        UE_LOG(LogInoAgents, Error, TEXT("UInoAgentsLiteRtLmAgentComponent: %s"), *Err);
        OnError.Broadcast(Err);
        return;
    }

    // Save to disk.
    const TArray<uint8>& Content = Response->GetContent();
    if (!FFileHelper::SaveArrayToFile(Content, *PendingDownloadTargetPath))
    {
        const FString Err = FString::Printf(
            TEXT("Failed to save model to %s"), *PendingDownloadTargetPath);
        UE_LOG(LogInoAgents, Error, TEXT("UInoAgentsLiteRtLmAgentComponent: %s"), *Err);
        OnError.Broadcast(Err);
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsLiteRtLmAgentComponent: model downloaded and saved to %s (%d bytes)"),
           *PendingDownloadTargetPath, Content.Num());

    // Now load the model normally.
    ULiteRtLmSubsystem* Subsys = SubsystemWeak.Get();
    if (Subsys == nullptr)
    {
        OnError.Broadcast(TEXT("Subsystem gone after download"));
        return;
    }

    FOnLiteRtLmModelLoaded OnLoaded;
    OnLoaded.BindDynamic(this, &UInoAgentsLiteRtLmAgentComponent::HandleModelLoaded);
    Subsys->LoadModelAsync(ModelConfig, OnLoaded);
}
