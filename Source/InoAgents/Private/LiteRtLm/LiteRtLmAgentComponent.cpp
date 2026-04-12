// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmAgentComponent.h"

#include "Audio/InoAgentsLiteRtLmDialogueQueue.h"
#include "Audio/InoAgentsStreamingAudioComponent.h"
#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmConversation.h"
#include "LiteRtLm/LiteRtLmSubsystem.h"

#include "Engine/GameInstance.h"
#include "GameFramework/Actor.h"

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

    // If the model is already loaded, skip straight to conversation creation.
    if (Subsys->IsModelLoaded())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("UInoAgentsLiteRtLmAgentComponent: model already loaded, "
                    "creating conversation directly"));
        HandleModelLoaded(true, FString());
        return;
    }

    // Forward download progress from the subsystem to our own delegate.
    Subsys->OnDownloadProgress.AddDynamic(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleDownloadProgress);

    // The subsystem handles everything: resolve path, download if
    // missing, then load. All we do is pass the config and a callback.
    FOnLiteRtLmModelLoaded OnLoaded;
    OnLoaded.BindDynamic(this, &UInoAgentsLiteRtLmAgentComponent::HandleModelLoaded);
    Subsys->LoadModelAsync(ModelConfig, OnLoaded);
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

// Download progress trampoline — forwards from subsystem to component delegate.
void UInoAgentsLiteRtLmAgentComponent::HandleDownloadProgress(
    float Percent, int64 BytesReceived, int64 TotalBytes)
{
    OnDownloadProgress.Broadcast(Percent, BytesReceived, TotalBytes);
}
