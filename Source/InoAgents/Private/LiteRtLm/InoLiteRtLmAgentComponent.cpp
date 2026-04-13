// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/InoLiteRtLmAgentComponent.h"

#include "Audio/InoLiteRtLmDialogueQueue.h"
#include "InoAgentsLog.h"
#include "LiteRtLm/InoLiteRtLmConversation.h"
#include "LiteRtLm/InoLiteRtLmSubsystem.h"

// RuntimeAudioImporter plugin
#include "Sound/StreamingSoundWave.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"

UInoLiteRtLmAgentComponent::UInoLiteRtLmAgentComponent(
    const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PrimaryComponentTick.bCanEverTick          = false;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    bAutoActivate = false;

    // TTS defaults.
    TtsRequestTemplate.ModelId      = TEXT("eleven_v3");
    TtsRequestTemplate.OutputFormat = EInoElevenLabsOutputFormat::Pcm_16000;
}

// ======================================================================
// Lifecycle
// ======================================================================

void UInoLiteRtLmAgentComponent::BeginPlay()
{
    Super::BeginPlay();

    // The agent no longer owns a UAudioComponent — Blueprint is
    // expected to grab the wave via GetStreamingSoundWave() and plug
    // it into whatever audio component it wants. We only own the
    // RuntimeAudioImporter UStreamingSoundWave; RuntimeAudio handles
    // all of decoding, threading, and clean PIE-shutdown teardown.
    //
    // Sample rate / channels / NumSamplesPerChunk are configured by
    // the dialogue queue's Initialize based on the ElevenLabs output
    // format (mono, rate from format, chunk = rate/100 for 10 ms
    // viseme cadence) — no need to set them here.
    StreamingWave = UStreamingSoundWave::CreateStreamingSoundWave();
}

void UInoLiteRtLmAgentComponent::EndPlay(EEndPlayReason::Type Reason)
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

    if (StreamingWave != nullptr)
    {
        // Stop any active source BP wired the wave into. Set the drain
        // flag FIRST so the next Parse() (which RuntimeAudio runs on
        // the audio thread per active sound) sees both conditions —
        // bStopSoundOnPlaybackFinish=true AND playback finished —
        // and calls AudioDevice->StopActiveSound. ReleaseMemory then
        // makes "playback finished" instantly true.
        StreamingWave->SetStopSoundOnPlaybackFinish(true);
        StreamingWave->ReleaseMemory();
        StreamingWave = nullptr;
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

void UInoLiteRtLmAgentComponent::Initialize(
    const FInoLiteRtLmModelConfig& InModelConfig,
    const FString& InVoiceId,
    const FInoElevenLabsDialogueRequest& InTtsRequestTemplate,
    int32 InPauseDurationMs,
    float InInterruptionDelaySec)
{
    ModelConfig            = InModelConfig;
    VoiceId                = InVoiceId;
    TtsRequestTemplate     = InTtsRequestTemplate;
    PauseDurationMs        = FMath::Max(InPauseDurationMs, 0);
    InterruptionDelaySec   = FMath::Clamp(InInterruptionDelaySec, 0.0f, 5.0f);

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoLiteRtLmAgentComponent::Initialize: model=%s, voice=%s, "
                "outputFmt=%d, pauseMs=%d"),
           *ModelConfig.ModelFileName, *VoiceId,
           static_cast<int32>(TtsRequestTemplate.OutputFormat),
           PauseDurationMs);
}

// ======================================================================
// LoadModel
// ======================================================================

void UInoLiteRtLmAgentComponent::LoadModel()
{
    if (ModelConfig.ModelFileName.IsEmpty())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoLiteRtLmAgentComponent::LoadModel: "
                    "ModelConfig.ModelFileName is empty."));
        OnError.Broadcast(TEXT("ModelConfig.ModelFileName is empty"));
        return;
    }

    const UGameInstance* GI = GetOwner() != nullptr
        ? GetOwner()->GetGameInstance()
        : nullptr;
    UInoLiteRtLmSubsystem* Subsys = GI != nullptr
        ? GI->GetSubsystem<UInoLiteRtLmSubsystem>()
        : nullptr;

    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoLiteRtLmAgentComponent::LoadModel: "
                    "no UInoLiteRtLmSubsystem — start PIE first."));
        OnError.Broadcast(TEXT("No UInoLiteRtLmSubsystem"));
        return;
    }
    SubsystemWeak = Subsys;

    if (Subsys->IsModelLoaded())
    {
        UE_LOG(LogInoAgents, Log,
               TEXT("UInoLiteRtLmAgentComponent: model already loaded"));
        HandleModelLoaded(true, FString());
        return;
    }

    Subsys->OnDownloadProgress.AddDynamic(
        this, &UInoLiteRtLmAgentComponent::HandleDownloadProgress);

    FOnInoLiteRtLmModelLoaded OnLoaded;
    OnLoaded.BindDynamic(this, &UInoLiteRtLmAgentComponent::HandleModelLoaded);
    Subsys->LoadModelAsync(ModelConfig, OnLoaded);
}

// ======================================================================
// Runtime API
// ======================================================================

void UInoLiteRtLmAgentComponent::SetEmotion(EInoEmotion NewEmotion)
{
    if (Emotion != NewEmotion)
    {
        Emotion = NewEmotion;
        OnEmotionChanged.Broadcast(NewEmotion);
    }
}

void UInoLiteRtLmAgentComponent::SetStatus(EInoAgentStatus NewStatus)
{
    if (Status != NewStatus)
    {
        Status = NewStatus;
        OnStatusChanged.Broadcast(NewStatus);
    }
}

void UInoLiteRtLmAgentComponent::SendMessage(const FString& Text)
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
    if (Status == EInoAgentStatus::Talking && InterruptionDelaySec > 0.0f)
    {
        SetStatus(EInoAgentStatus::Interrupted);
        PendingInterruptMessage = Text;

        if (UWorld* World = GetWorld())
        {
            World->GetTimerManager().ClearTimer(InterruptionTimerHandle);
            World->GetTimerManager().SetTimer(
                InterruptionTimerHandle,
                this,
                &UInoLiteRtLmAgentComponent::OnInterruptionDelayFinished,
                InterruptionDelaySec,
                /*bLoop=*/ false);
        }
        return;
    }

    // No delay (or wasn't talking) — go straight to Thinking.
    if (Status == EInoAgentStatus::Talking)
    {
        SetStatus(EInoAgentStatus::Interrupted);
    }
    SetStatus(EInoAgentStatus::Thinking);
    Conversation->SendMessageAsync(Text);
}

void UInoLiteRtLmAgentComponent::OnInterruptionDelayFinished()
{
    if (Conversation == nullptr)
    {
        SetStatus(EInoAgentStatus::Idle);
        return;
    }

    const FString Message = MoveTemp(PendingInterruptMessage);
    SetStatus(EInoAgentStatus::Thinking);
    Conversation->SendMessageAsync(Message);
}

void UInoLiteRtLmAgentComponent::Cancel()
{
    // Tear down in-flight TTS audio FIRST so the old response doesn't
    // keep playing past the cancel. Without this, Cancel stops the
    // LLM generation but leaves the dialogue queue's slots alive; the
    // wave would drain whatever TTS had already started.
    if (DialogueQueue != nullptr)
    {
        DialogueQueue->StopAndReset();
    }

    if (Conversation != nullptr)
    {
        Conversation->Cancel();
    }
}

void UInoLiteRtLmAgentComponent::ShowChatPanel()
{
    if (UInoLiteRtLmSubsystem* Subsys = SubsystemWeak.Get())
    {
        Subsys->ShowChatPanel(Conversation);
    }
}

void UInoLiteRtLmAgentComponent::HideChatPanel()
{
    if (UInoLiteRtLmSubsystem* Subsys = SubsystemWeak.Get())
    {
        Subsys->HideChatPanel();
    }
}

void UInoLiteRtLmAgentComponent::ClearDialogueQueue()
{
    if (DialogueQueue != nullptr)
    {
        DialogueQueue->Clear();
    }
}

bool UInoLiteRtLmAgentComponent::IsModelLoaded() const
{
    if (const UInoLiteRtLmSubsystem* Subsys = SubsystemWeak.Get())
    {
        return Subsys->IsModelLoaded();
    }
    return false;
}

bool UInoLiteRtLmAgentComponent::IsStreaming() const
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

void UInoLiteRtLmAgentComponent::HandleModelLoaded(
    bool bSuccess, FString ErrorMessage)
{
    if (!bSuccess)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoLiteRtLmAgentComponent: model load FAILED: %s"),
               *ErrorMessage);
        OnModelLoaded.Broadcast(false, ErrorMessage);
        OnError.Broadcast(FString::Printf(TEXT("Model load failed: %s"), *ErrorMessage));
        return;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoLiteRtLmAgentComponent: model loaded, creating conversation"));

    CreateConversationAndQueue();
    OnModelLoaded.Broadcast(true, FString());
}

void UInoLiteRtLmAgentComponent::HandleToken(FString RawText, FString CleanText)
{
    OnToken.Broadcast(RawText, CleanText);
}

namespace
{
    bool TryParseEmotion(const FString& Text, EInoEmotion& OutEmotion)
    {
        // Look for {emotion} tag in the text.
        int32 Open = Text.Find(TEXT("{"));
        int32 Close = Text.Find(TEXT("}"));
        if (Open == INDEX_NONE || Close == INDEX_NONE || Close <= Open)
        {
            return false;
        }

        const FString Tag = Text.Mid(Open + 1, Close - Open - 1).ToLower().TrimStartAndEnd();

        if (Tag == TEXT("neutral"))   { OutEmotion = EInoEmotion::Neutral;   return true; }
        if (Tag == TEXT("happy"))     { OutEmotion = EInoEmotion::Happy;     return true; }
        if (Tag == TEXT("sad"))       { OutEmotion = EInoEmotion::Sad;       return true; }
        if (Tag == TEXT("disgust"))   { OutEmotion = EInoEmotion::Disgust;   return true; }
        if (Tag == TEXT("anger") || Tag == TEXT("angry"))
                                     { OutEmotion = EInoEmotion::Anger;     return true; }
        if (Tag == TEXT("surprise") || Tag == TEXT("surprised"))
                                     { OutEmotion = EInoEmotion::Surprise;  return true; }
        if (Tag == TEXT("fear"))      { OutEmotion = EInoEmotion::Fear;      return true; }
        if (Tag == TEXT("confident")) { OutEmotion = EInoEmotion::Confident; return true; }
        if (Tag == TEXT("excited"))   { OutEmotion = EInoEmotion::Excited;   return true; }
        if (Tag == TEXT("bored"))     { OutEmotion = EInoEmotion::Bored;     return true; }
        if (Tag == TEXT("playful"))   { OutEmotion = EInoEmotion::Playful;   return true; }
        if (Tag == TEXT("confused"))  { OutEmotion = EInoEmotion::Confused;  return true; }

        return false;
    }
}

void UInoLiteRtLmAgentComponent::HandleSentence(FString RawText, FString CleanText)
{
    // Detect {emotion} tag in the raw text and update emotion state.
    EInoEmotion DetectedEmotion;
    if (TryParseEmotion(RawText, DetectedEmotion))
    {
        SetEmotion(DetectedEmotion);
    }

    OnSentence.Broadcast(RawText, CleanText);
}

void UInoLiteRtLmAgentComponent::HandleSentenceBoundary()
{
    OnSentenceBoundary.Broadcast();
}

void UInoLiteRtLmAgentComponent::HandleComplete(FString FullText)
{
    OnComplete.Broadcast(FullText);
}

void UInoLiteRtLmAgentComponent::HandleError(FString ErrorMessage)
{
    // LLM error mid-response — drop any TTS slots in flight so the
    // queue doesn't keep feeding the wave with partial bytes from a
    // dead conversation. The agent goes Idle; BP sees OnError and
    // can show an error state.
    if (DialogueQueue != nullptr)
    {
        DialogueQueue->StopAndReset();
    }

    bTalkingLatched = false;
    SetStatus(EInoAgentStatus::Idle);
    OnError.Broadcast(ErrorMessage);
}

void UInoLiteRtLmAgentComponent::HandleToolCalled(
    FName ToolName, FString ArgumentsJson, FString ResultJson)
{
    OnToolCalled.Broadcast(ToolName, ArgumentsJson, ResultJson);
}

void UInoLiteRtLmAgentComponent::HandleWavePopulateAudioData(
    const TArray<float>& /*PopulatedAudioData*/)
{
    // The streaming wave fires OnPopulateAudioData each time the queue
    // appends a fresh chunk. Treat the first appearance after a
    // SendMessage as the "audio is about to play" edge — that's when
    // we flip from Thinking to Talking.
    if (!bTalkingLatched)
    {
        bTalkingLatched = true;
        SetStatus(EInoAgentStatus::Talking);
    }
}

void UInoLiteRtLmAgentComponent::HandleAudioPlaybackFinished()
{
    // Wave has fully drained — the queue set SetStopSoundOnPlaybackFinish
    // when its last slot was dispatched, so this delegate firing is
    // our authoritative "all audio played" signal.
    bTalkingLatched = false;
    SetStatus(EInoAgentStatus::Idle);
    OnAudioFinished.Broadcast();
}


void UInoLiteRtLmAgentComponent::HandleDownloadProgress(
    float Percent, int64 BytesReceived, int64 TotalBytes)
{
    OnDownloadProgress.Broadcast(Percent, BytesReceived, TotalBytes);
}

// ======================================================================
// Internal
// ======================================================================

void UInoLiteRtLmAgentComponent::CreateConversationAndQueue()
{
    UInoLiteRtLmSubsystem* Subsys = SubsystemWeak.Get();
    if (Subsys == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("UInoLiteRtLmAgentComponent: subsystem gone"));
        return;
    }

    // Create conversation.
    Conversation = Subsys->CreateConversation();
    if (Conversation == nullptr)
    {
        OnError.Broadcast(TEXT("CreateConversation returned null"));
        return;
    }

    // Apply sentence-split flags up front so the first streamed tokens
    // already use the caller's configured boundaries.
    Conversation->SetSentenceSplitFlags(SentenceSplitFlags);

    // Bind ALL conversation delegates → trampolines.
    Conversation->OnToken.AddDynamic(
        this, &UInoLiteRtLmAgentComponent::HandleToken);
    Conversation->OnSentence.AddDynamic(
        this, &UInoLiteRtLmAgentComponent::HandleSentence);
    Conversation->OnSentenceBoundary.AddDynamic(
        this, &UInoLiteRtLmAgentComponent::HandleSentenceBoundary);
    Conversation->OnComplete.AddDynamic(
        this, &UInoLiteRtLmAgentComponent::HandleComplete);
    Conversation->OnError.AddDynamic(
        this, &UInoLiteRtLmAgentComponent::HandleError);
    Conversation->OnToolCalled.AddDynamic(
        this, &UInoLiteRtLmAgentComponent::HandleToolCalled);

    // Bind wave-level delegates for status transitions. Format
    // (rate / channels / chunk size) is configured by the dialogue
    // queue's Initialize below — no need to apply it here.
    if (StreamingWave != nullptr)
    {
        StreamingWave->OnPopulateAudioData.AddDynamic(
            this, &UInoLiteRtLmAgentComponent::HandleWavePopulateAudioData);
        StreamingWave->OnAudioPlaybackFinished.AddDynamic(
            this, &UInoLiteRtLmAgentComponent::HandleAudioPlaybackFinished);
    }

    // Create and initialize the dialogue queue.
    DialogueQueue = NewObject<UInoLiteRtLmDialogueQueue>(this);
    DialogueQueue->Initialize(
        this,
        StreamingWave,
        Conversation,
        VoiceId,
        TtsRequestTemplate,
        PauseDurationMs);

    UE_LOG(LogInoAgents, Log,
           TEXT("UInoLiteRtLmAgentComponent: ready (conversation=%s, voice=%s)"),
           *Conversation->GetName(), *VoiceId);
}
