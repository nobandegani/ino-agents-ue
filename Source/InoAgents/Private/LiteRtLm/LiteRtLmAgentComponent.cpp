// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmAgentComponent.h"

#include "Audio/InoAgentsLiteRtLmDialogueQueue.h"
#include "Audio/InoAgentsStreamingAudioComponent.h"
#include "InoAgentsLog.h"
#include "LiteRtLm/LiteRtLmConversation.h"
#include "LiteRtLm/LiteRtLmSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"

UInoAgentsLiteRtLmAgentComponent::UInoAgentsLiteRtLmAgentComponent(
    const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PrimaryComponentTick.bCanEverTick          = false;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    bAutoActivate = false;

    // Child audio component for spatialised playback.
    AudioComp = ObjectInitializer.CreateDefaultSubobject<UInoAgentsStreamingAudioComponent>(
        this, TEXT("StreamingAudio"));
    if (AudioComp != nullptr)
    {
        AudioComp->SetupAttachment(this);
        // Match the default ElevenLabs PCM output: 16 kHz mono.
        AudioComp->SetPcmFormat(16000, 1);
    }

    // TTS defaults.
    TtsRequestTemplate.ModelId      = TEXT("eleven_v3");
    TtsRequestTemplate.OutputFormat = EElevenLabsOutputFormat::Pcm_16000;
}

// ======================================================================
// Lifecycle
// ======================================================================

void UInoAgentsLiteRtLmAgentComponent::BeginPlay()
{
    Super::BeginPlay();
}

void UInoAgentsLiteRtLmAgentComponent::EndPlay(EEndPlayReason::Type Reason)
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(InterruptionTimerHandle);
    }

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
// Setup API
// ======================================================================

void UInoAgentsLiteRtLmAgentComponent::Initialize(
    const FLiteRtLmModelConfig& InModelConfig,
    const FString& InVoiceId,
    const FElevenLabsDialogueRequest& InTtsRequestTemplate,
    int32 InPauseDurationMs,
    int32 InPreBufferMs,
    int32 InPcmSampleRate,
    int32 InPcmNumChannels)
{
    ModelConfig        = InModelConfig;
    VoiceId            = InVoiceId;
    TtsRequestTemplate = InTtsRequestTemplate;
    PauseDurationMs    = FMath::Max(InPauseDurationMs, 0);
    PreBufferMs        = FMath::Clamp(InPreBufferMs, 0, 2000);
    PcmSampleRate      = FMath::Clamp(InPcmSampleRate, 8000, 192000);
    PcmNumChannels     = FMath::Clamp(InPcmNumChannels, 1, 2);

    if (AudioComp != nullptr)
    {
        AudioComp->SetPcmFormat(PcmSampleRate, PcmNumChannels);
        AudioComp->PreBufferMs = PreBufferMs;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsLiteRtLmAgentComponent::Initialize: model=%s, voice=%s, "
                "outputFmt=%d, pauseMs=%d, pcm=%dHz/%dch"),
           *ModelConfig.ModelFileName, *VoiceId,
           static_cast<int32>(TtsRequestTemplate.OutputFormat),
           PauseDurationMs, PcmSampleRate, PcmNumChannels);
}

// ======================================================================
// LoadModel
// ======================================================================

void UInoAgentsLiteRtLmAgentComponent::LoadModel()
{
    if (ModelConfig.ModelFileName.IsEmpty())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsLiteRtLmAgentComponent::LoadModel: "
                    "ModelConfig.ModelFileName is empty."));
        OnError.Broadcast(TEXT("ModelConfig.ModelFileName is empty"));
        return;
    }

    const UGameInstance* GI = GetOwner() != nullptr
        ? GetOwner()->GetGameInstance()
        : nullptr;
    ULiteRtLmSubsystem* Subsys = GI != nullptr
        ? GI->GetSubsystem<ULiteRtLmSubsystem>()
        : nullptr;

    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoAgentsLiteRtLmAgentComponent::LoadModel: "
                    "no ULiteRtLmSubsystem — start PIE first."));
        OnError.Broadcast(TEXT("No ULiteRtLmSubsystem"));
        return;
    }
    SubsystemWeak = Subsys;

    if (Subsys->IsModelLoaded())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("UInoAgentsLiteRtLmAgentComponent: model already loaded"));
        HandleModelLoaded(true, FString());
        return;
    }

    Subsys->OnDownloadProgress.AddDynamic(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleDownloadProgress);

    FOnLiteRtLmModelLoaded OnLoaded;
    OnLoaded.BindDynamic(this, &UInoAgentsLiteRtLmAgentComponent::HandleModelLoaded);
    Subsys->LoadModelAsync(ModelConfig, OnLoaded);
}

// ======================================================================
// Runtime API
// ======================================================================

void UInoAgentsLiteRtLmAgentComponent::SetStatus(EInoAgentsAgentStatus NewStatus)
{
    if (Status != NewStatus)
    {
        Status = NewStatus;
        OnStatusChanged.Broadcast(NewStatus);
    }
}

void UInoAgentsLiteRtLmAgentComponent::SendMessage(const FString& Text)
{
    if (Conversation == nullptr)
    {
        OnError.Broadcast(TEXT("No conversation — call LoadModel first"));
        return;
    }

    // Stop any in-progress audio from the previous response.
    if (DialogueQueue != nullptr)
    {
        DialogueQueue->StopAndReset();
    }

    // If the agent was talking, mark as interrupted. If there's a
    // delay configured, hold in Interrupted state for that duration
    // before transitioning to Thinking and sending the message.
    if (Status == EInoAgentsAgentStatus::Talking && InterruptionDelaySec > 0.0f)
    {
        SetStatus(EInoAgentsAgentStatus::Interrupted);
        PendingInterruptMessage = Text;

        if (UWorld* World = GetWorld())
        {
            World->GetTimerManager().ClearTimer(InterruptionTimerHandle);
            World->GetTimerManager().SetTimer(
                InterruptionTimerHandle,
                this,
                &UInoAgentsLiteRtLmAgentComponent::OnInterruptionDelayFinished,
                InterruptionDelaySec,
                /*bLoop=*/ false);
        }
        return;
    }

    // No delay (or wasn't talking) — go straight to Thinking.
    if (Status == EInoAgentsAgentStatus::Talking)
    {
        SetStatus(EInoAgentsAgentStatus::Interrupted);
    }
    SetStatus(EInoAgentsAgentStatus::Thinking);
    Conversation->SendMessageAsync(Text);
}

void UInoAgentsLiteRtLmAgentComponent::OnInterruptionDelayFinished()
{
    if (Conversation == nullptr)
    {
        SetStatus(EInoAgentsAgentStatus::Idle);
        return;
    }

    const FString Message = MoveTemp(PendingInterruptMessage);
    SetStatus(EInoAgentsAgentStatus::Thinking);
    Conversation->SendMessageAsync(Message);
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

void UInoAgentsLiteRtLmAgentComponent::ClearDialogueQueue()
{
    if (DialogueQueue != nullptr)
    {
        DialogueQueue->Clear();
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

void UInoAgentsLiteRtLmAgentComponent::HandleToken(FString RawText, FString CleanText)
{
    OnToken.Broadcast(RawText, CleanText);
}

void UInoAgentsLiteRtLmAgentComponent::HandleSentence(FString RawText, FString CleanText)
{
    OnSentence.Broadcast(RawText, CleanText);
}

void UInoAgentsLiteRtLmAgentComponent::HandleNewLine()
{
    OnNewLine.Broadcast();
}

void UInoAgentsLiteRtLmAgentComponent::HandleComplete(FString FullText)
{
    OnComplete.Broadcast(FullText);
}

void UInoAgentsLiteRtLmAgentComponent::HandleError(FString ErrorMessage)
{
    SetStatus(EInoAgentsAgentStatus::Idle);
    OnError.Broadcast(ErrorMessage);
}

void UInoAgentsLiteRtLmAgentComponent::HandleToolCalled(
    FName ToolName, FString ArgumentsJson, FString ResultJson)
{
    OnToolCalled.Broadcast(ToolName, ArgumentsJson, ResultJson);
}

void UInoAgentsLiteRtLmAgentComponent::HandleAudioReadyToPlay()
{
    SetStatus(EInoAgentsAgentStatus::Talking);
}

void UInoAgentsLiteRtLmAgentComponent::HandleAudioPlaybackFinished()
{
    // Audio component has fully drained — no more sound playing.
    SetStatus(EInoAgentsAgentStatus::Idle);
    OnAudioFinished.Broadcast();
}

void UInoAgentsLiteRtLmAgentComponent::HandleAudioFinished()
{
    // DialogueQueue::OnAllComplete — all TTS slots processed and audio
    // queued. Audio may still be playing. Don't set Idle here — wait
    // for HandleAudioPlaybackFinished (AudioComp::OnFinished) instead.
}

void UInoAgentsLiteRtLmAgentComponent::HandleDownloadProgress(
    float Percent, int64 BytesReceived, int64 TotalBytes)
{
    OnDownloadProgress.Broadcast(Percent, BytesReceived, TotalBytes);
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
               TEXT("UInoAgentsLiteRtLmAgentComponent: subsystem gone"));
        return;
    }

    // Create conversation.
    Conversation = Subsys->CreateConversation();
    if (Conversation == nullptr)
    {
        OnError.Broadcast(TEXT("CreateConversation returned null"));
        return;
    }

    // Bind ALL conversation delegates → trampolines.
    Conversation->OnToken.AddDynamic(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleToken);
    Conversation->OnSentence.AddDynamic(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleSentence);
    Conversation->OnNewLine.AddDynamic(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleNewLine);
    Conversation->OnComplete.AddDynamic(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleComplete);
    Conversation->OnError.AddDynamic(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleError);
    Conversation->OnToolCalled.AddDynamic(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleToolCalled);

    // Apply PCM format from config (handles the case where Initialize
    // wasn't called and LoadModel uses details-panel defaults).
    if (AudioComp != nullptr)
    {
        AudioComp->SetPcmFormat(PcmSampleRate, PcmNumChannels);
        AudioComp->PreBufferMs = PreBufferMs;
        AudioComp->OnReadyToPlay.AddDynamic(
            this, &UInoAgentsLiteRtLmAgentComponent::HandleAudioReadyToPlay);
        AudioComp->OnFinished.AddDynamic(
            this, &UInoAgentsLiteRtLmAgentComponent::HandleAudioPlaybackFinished);
    }

    // Create and initialize the dialogue queue.
    DialogueQueue = NewObject<UInoAgentsLiteRtLmDialogueQueue>(this);
    DialogueQueue->Initialize(
        this,
        AudioComp,
        Conversation,
        VoiceId,
        TtsRequestTemplate,
        PauseDurationMs);

    DialogueQueue->OnAllComplete.AddDynamic(
        this, &UInoAgentsLiteRtLmAgentComponent::HandleAudioFinished);

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsLiteRtLmAgentComponent: ready (conversation=%s, voice=%s)"),
           *Conversation->GetName(), *VoiceId);
}
