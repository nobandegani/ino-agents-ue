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

    // Create the audio component at runtime and attach to the owning
    // actor's root. Using NewObject + RegisterComponent (not
    // CreateDefaultSubobject) so it doesn't show as a duplicate in
    // the details panel — the agent is an UActorComponent with no
    // transform, the audio component is just an implementation detail.
    AActor* Owner = GetOwner();
    if (Owner != nullptr)
    {
        AudioComp = NewObject<UInoAgentsStreamingAudioComponent>(Owner, TEXT("AgentStreamingAudio"));
        AudioComp->SetPcmFormat(PcmSampleRate, PcmNumChannels);
        AudioComp->SetupAttachment(Owner->GetRootComponent());
        AudioComp->RegisterComponent();
    }
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

    if (AudioComp != nullptr)
    {
        AudioComp->StopAndReset();
        AudioComp->DestroyComponent();
        AudioComp = nullptr;
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
    float InInterruptionDelaySec,
    int32 InPreBufferMs,
    int32 InPcmSampleRate,
    int32 InPcmNumChannels)
{
    ModelConfig            = InModelConfig;
    VoiceId                = InVoiceId;
    TtsRequestTemplate     = InTtsRequestTemplate;
    PauseDurationMs        = FMath::Max(InPauseDurationMs, 0);
    InterruptionDelaySec   = FMath::Clamp(InInterruptionDelaySec, 0.0f, 5.0f);
    PreBufferMs            = FMath::Clamp(InPreBufferMs, 0, 2000);
    PcmSampleRate          = FMath::Clamp(InPcmSampleRate, 8000, 192000);
    PcmNumChannels         = FMath::Clamp(InPcmNumChannels, 1, 2);

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

void UInoAgentsLiteRtLmAgentComponent::SetEmotion(EInoAgentsEmotion NewEmotion)
{
    if (Emotion != NewEmotion)
    {
        Emotion = NewEmotion;
        OnEmotionChanged.Broadcast(NewEmotion);
    }
}

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

namespace
{
    bool TryParseEmotion(const FString& Text, EInoAgentsEmotion& OutEmotion)
    {
        // Look for {emotion} tag in the text.
        int32 Open = Text.Find(TEXT("{"));
        int32 Close = Text.Find(TEXT("}"));
        if (Open == INDEX_NONE || Close == INDEX_NONE || Close <= Open)
        {
            return false;
        }

        const FString Tag = Text.Mid(Open + 1, Close - Open - 1).ToLower().TrimStartAndEnd();

        if (Tag == TEXT("neutral"))   { OutEmotion = EInoAgentsEmotion::Neutral;   return true; }
        if (Tag == TEXT("happy"))     { OutEmotion = EInoAgentsEmotion::Happy;     return true; }
        if (Tag == TEXT("sad"))       { OutEmotion = EInoAgentsEmotion::Sad;       return true; }
        if (Tag == TEXT("disgust"))   { OutEmotion = EInoAgentsEmotion::Disgust;   return true; }
        if (Tag == TEXT("anger") || Tag == TEXT("angry"))
                                     { OutEmotion = EInoAgentsEmotion::Anger;     return true; }
        if (Tag == TEXT("surprise") || Tag == TEXT("surprised"))
                                     { OutEmotion = EInoAgentsEmotion::Surprise;  return true; }
        if (Tag == TEXT("fear"))      { OutEmotion = EInoAgentsEmotion::Fear;      return true; }
        if (Tag == TEXT("confident")) { OutEmotion = EInoAgentsEmotion::Confident; return true; }
        if (Tag == TEXT("excited"))   { OutEmotion = EInoAgentsEmotion::Excited;   return true; }
        if (Tag == TEXT("bored"))     { OutEmotion = EInoAgentsEmotion::Bored;     return true; }
        if (Tag == TEXT("playful"))   { OutEmotion = EInoAgentsEmotion::Playful;   return true; }
        if (Tag == TEXT("confused"))  { OutEmotion = EInoAgentsEmotion::Confused;  return true; }

        return false;
    }
}

void UInoAgentsLiteRtLmAgentComponent::HandleSentence(FString RawText, FString CleanText)
{
    // Detect {emotion} tag in the raw text and update emotion state.
    EInoAgentsEmotion DetectedEmotion;
    if (TryParseEmotion(RawText, DetectedEmotion))
    {
        SetEmotion(DetectedEmotion);
    }

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

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsLiteRtLmAgentComponent: ready (conversation=%s, voice=%s)"),
           *Conversation->GetName(), *VoiceId);
}
