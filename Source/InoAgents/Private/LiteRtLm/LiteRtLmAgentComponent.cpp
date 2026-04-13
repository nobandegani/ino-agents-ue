// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmAgentComponent.h"

#include "Audio/InoAgentsLiteRtLmDialogueQueue.h"
#include "Audio/InoAgentsStreamingSoundWave.h"
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

    // The agent no longer owns a UAudioComponent — Blueprint is
    // expected to grab the wave via GetStreamingSoundWave() and
    // plug it into whatever audio component it wants to use
    // (spatialized, 2D UI, routed through a specific sound class,
    // etc.). We only own the wave and its format config.
    StreamingWave = UInoAgentsStreamingSoundWave::CreateStreamingSoundWave();
    StreamingWave->SetInitialDesiredSampleRate(PcmSampleRate);
    StreamingWave->SetInitialDesiredNumChannels(PcmNumChannels);
    StreamingWave->SetNumSamplesPerChunk(NumSamplesPerChunk);
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

    StreamingWave = nullptr;

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
    int32 InPcmSampleRate,
    int32 InPcmNumChannels)
{
    ModelConfig            = InModelConfig;
    VoiceId                = InVoiceId;
    TtsRequestTemplate     = InTtsRequestTemplate;
    PauseDurationMs        = FMath::Max(InPauseDurationMs, 0);
    InterruptionDelaySec   = FMath::Clamp(InInterruptionDelaySec, 0.0f, 5.0f);
    PcmSampleRate          = FMath::Clamp(InPcmSampleRate, 8000, 192000);
    PcmNumChannels         = FMath::Clamp(InPcmNumChannels, 1, 2);

    if (StreamingWave != nullptr)
    {
        StreamingWave->SetInitialDesiredSampleRate(PcmSampleRate);
        StreamingWave->SetInitialDesiredNumChannels(PcmNumChannels);
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

    // Each new message gets a fresh "first data → Talking" edge.
    bTalkingLatched = false;

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

void UInoAgentsLiteRtLmAgentComponent::HandleWavePopulateAudioData(
    const TArray<float>& /*PopulatedAudioData*/)
{
    // The streaming wave fires OnPopulateAudioData each time the queue
    // appends a fresh chunk. Treat the first appearance after a
    // SendMessage as the "audio is about to play" edge — that's when
    // we flip from Thinking to Talking.
    if (!bTalkingLatched)
    {
        bTalkingLatched = true;
        SetStatus(EInoAgentsAgentStatus::Talking);
    }
}

void UInoAgentsLiteRtLmAgentComponent::HandleAudioPlaybackFinished()
{
    // Wave has fully drained — the queue set SetStopSoundOnPlaybackFinish
    // when its last slot was dispatched, so this delegate firing is
    // our authoritative "all audio played" signal.
    bTalkingLatched = false;
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

    // Bind wave-level delegates for status transitions. The wave may
    // have been re-created if the user tore down the actor and
    // re-spawned; re-apply the PCM format each time.
    if (StreamingWave != nullptr)
    {
        StreamingWave->SetInitialDesiredSampleRate(PcmSampleRate);
        StreamingWave->SetInitialDesiredNumChannels(PcmNumChannels);
        StreamingWave->SetNumSamplesPerChunk(NumSamplesPerChunk);
        StreamingWave->OnPopulateAudioData.AddDynamic(
            this, &UInoAgentsLiteRtLmAgentComponent::HandleWavePopulateAudioData);
        StreamingWave->OnAudioPlaybackFinished.AddDynamic(
            this, &UInoAgentsLiteRtLmAgentComponent::HandleAudioPlaybackFinished);
    }

    // Create and initialize the dialogue queue.
    DialogueQueue = NewObject<UInoAgentsLiteRtLmDialogueQueue>(this);
    DialogueQueue->Initialize(
        this,
        StreamingWave,
        Conversation,
        VoiceId,
        TtsRequestTemplate,
        PauseDurationMs);

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoAgentsLiteRtLmAgentComponent: ready (conversation=%s, voice=%s)"),
           *Conversation->GetName(), *VoiceId);
}
