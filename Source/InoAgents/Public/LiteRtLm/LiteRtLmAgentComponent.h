// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "Audio/InoAgentsAudioTypes.h"
#include "ElevenLabs/ElevenLabsTypes.h"
#include "LiteRtLm/LiteRtLmTypes.h"

#include "LiteRtLmAgentComponent.generated.h"

class UInoAgentsStreamingAudioComponent;
class UInoAgentsLiteRtLmDialogueQueue;
class ULiteRtLmConversation;
class ULiteRtLmSubsystem;

/** Agent activity state. */
UENUM(BlueprintType)
enum class EInoAgentsAgentStatus : uint8
{
    /** No active request. Ready for input. */
    Idle        UMETA(DisplayName = "Idle"),
    /** Model is generating a response. */
    Thinking    UMETA(DisplayName = "Thinking"),
    /** TTS audio is playing the response. */
    Talking     UMETA(DisplayName = "Talking"),
    /** User sent a new message while the agent was talking. */
    Interrupted UMETA(DisplayName = "Interrupted"),
    /** Reserved for future STT / voice input. */
    Listening   UMETA(DisplayName = "Listening"),
};

/** Multicast version of FOnLiteRtLmModelLoaded for BlueprintAssignable. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInoAgentsAgentModelLoaded,
    bool, bSuccess,
    FString, ErrorMessage);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInoAgentsAgentStatusChanged,
    EInoAgentsAgentStatus, NewStatus);

/**
 * All-in-one LiteRT-LM + ElevenLabs agent component.
 *
 * Drop on any actor → Initialize → LoadModel → SendMessage.
 *
 * Blueprint usage:
 *   BeginPlay:
 *     Agent → Initialize (ModelConfig, VoiceId, TtsTemplate, PauseMs)
 *     Agent → Load Model
 *   On trigger:
 *     Agent → Send Message ("Hello")
 *   Bind events:
 *     OnToken, OnSentence, OnComplete, OnError, OnAudioFinished, etc.
 */
UCLASS(ClassGroup = (InoAgents),
       meta = (BlueprintSpawnableComponent, DisplayName = "LiteRT-LM Agent"))
class INOAGENTS_API UInoAgentsLiteRtLmAgentComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UInoAgentsLiteRtLmAgentComponent(const FObjectInitializer& ObjectInitializer);

    // =============================================================
    // Configuration (editable in details panel as defaults)
    // =============================================================

    /** Model configuration — ModelFileName, Backend, SystemMessage. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent")
    FLiteRtLmModelConfig ModelConfig;

    /** ElevenLabs voice ID. Default: EwVlpfIFmNJ50rqcxXfJ. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent")
    FString VoiceId = TEXT("EwVlpfIFmNJ50rqcxXfJ");

    /** ElevenLabs TTS settings. Default: model=eleven_v3, format=Pcm_16000. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent")
    FElevenLabsDialogueRequest TtsRequestTemplate;

    /** Silence in ms between lines. 0 = no pause. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent",
              meta = (ClampMin = "0", ClampMax = "5000"))
    int32 PauseDurationMs = 500;

    /** How long the Interrupted status lasts (seconds) before
     *  transitioning to Thinking. 0 = instant (no delay). Use this
     *  to play an interruption animation or sound effect. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent",
              meta = (ClampMin = "0.0", ClampMax = "5.0"))
    float InterruptionDelaySec = 0.0f;

    /** How many ms of audio to buffer before starting playback.
     *  Higher = smoother start, lower = faster first word.
     *  Set via Initialize or directly on the child audio component. */
    UPROPERTY(BlueprintReadWrite, Category = "InoAgents|Agent|Audio",
              meta = (ClampMin = "0", ClampMax = "2000"))
    int32 PreBufferMs = 250;

    /** PCM sample rate for the audio component. Must match the
     *  ElevenLabs output format (e.g. 16000 for Pcm_16000). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent|Audio",
              meta = (ClampMin = "8000", ClampMax = "192000"))
    int32 PcmSampleRate = 16000;

    /** PCM channel count. 1 = mono, 2 = stereo. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent|Audio",
              meta = (ClampMin = "1", ClampMax = "2"))
    int32 PcmNumChannels = 1;

    // =============================================================
    // Setup API
    // =============================================================

    /**
     * Configure all settings and prepare the dialogue queue.
     * Call once before LoadModel. Overrides any values set in the
     * details panel.
     *
     * Can be skipped if you set values in the details panel instead
     * — LoadModel will use the UPROPERTY values directly.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Agent")
    void Initialize(
        const FLiteRtLmModelConfig& InModelConfig,
        const FString& InVoiceId,
        const FElevenLabsDialogueRequest& InTtsRequestTemplate,
        int32 InPauseDurationMs,
        float InInterruptionDelaySec,
        int32 InPreBufferMs,
        int32 InPcmSampleRate,
        int32 InPcmNumChannels);

    /**
     * Load the model and create a conversation. Non-blocking.
     * OnModelLoaded fires when done. Uses the config set via
     * Initialize or the details panel defaults. Safe to call
     * multiple times — subsequent calls after a successful load
     * are no-ops.
     */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Agent")
    void LoadModel();

    // =============================================================
    // Runtime API
    // =============================================================

    // =============================================================
    // Runtime API
    // =============================================================

    /** Send a user message. Context set on the conversation via
     *  SetSystemContext / SetUserContext is automatically merged
     *  and passed to the model. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Agent")
    void SendMessage(const FString& Text);

    /** Cancel the in-flight response. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Agent")
    void Cancel();

    /** Show the debug Slate chat panel connected to this conversation. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Agent")
    void ShowChatPanel();

    /** Hide the debug chat panel. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Agent")
    void HideChatPanel();

    /** Clear the TTS dialogue queue (stop all pending audio). */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Agent")
    void ClearDialogueQueue();

    // =============================================================
    // Delegates — conversation
    // =============================================================

    /** Fires when LoadModel completes (success or failure). */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoAgentsAgentModelLoaded OnModelLoaded;

    /** Fires per streaming token from the model. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnLiteRtLmToken OnToken;

    /** Fires per newline-delimited line. RawText has [emotion] tags;
     *  CleanText has them stripped for UI. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnLiteRtLmSentence OnSentence;

    /** Fires at each newline boundary (after OnSentence). */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnLiteRtLmNewLine OnNewLine;

    /** Fires once when the model finishes its full response. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnLiteRtLmComplete OnComplete;

    /** Fires once on any error. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnLiteRtLmError OnError;

    /** Fires once per tool call executed (diagnostic). */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnLiteRtLmToolCalled OnToolCalled;

    // =============================================================
    // Delegates — dialogue queue / audio
    // =============================================================

    /** Fires when all queued TTS audio has finished playing. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoAgentsAudioFinished OnAudioFinished;

    /** Fires during model download. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoAgentsModelDownloadProgress OnDownloadProgress;

    // =============================================================
    // Delegates — status
    // =============================================================

    /** Fires whenever the agent's status changes (Idle, Thinking,
     *  Talking, Interrupted, Listening). */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoAgentsAgentStatusChanged OnStatusChanged;

    // =============================================================
    // Read-only state
    // =============================================================

    /** Current agent status. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    EInoAgentsAgentStatus GetStatus() const { return Status; }

    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    bool IsModelLoaded() const;

    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    bool IsStreaming() const;

    /** Access the internal conversation for advanced use. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    ULiteRtLmConversation* GetConversation() const { return Conversation; }

    /** Access the child audio component. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    UInoAgentsStreamingAudioComponent* GetAudioComponent() const { return AudioComp; }

    /** Access the internal dialogue queue. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    UInoAgentsLiteRtLmDialogueQueue* GetDialogueQueue() const { return DialogueQueue; }

    //~ UActorComponent interface
    virtual void BeginPlay() override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
    //~ End UActorComponent interface

private:
    UPROPERTY()
    TObjectPtr<UInoAgentsStreamingAudioComponent> AudioComp;

    UPROPERTY()
    TObjectPtr<ULiteRtLmConversation> Conversation;

    UPROPERTY()
    TObjectPtr<UInoAgentsLiteRtLmDialogueQueue> DialogueQueue;

    TWeakObjectPtr<ULiteRtLmSubsystem> SubsystemWeak;

    EInoAgentsAgentStatus Status = EInoAgentsAgentStatus::Idle;

    /** Set status and broadcast OnStatusChanged if it actually changed. */
    void SetStatus(EInoAgentsAgentStatus NewStatus);

    /** Pending message held during interruption delay. */
    FString PendingInterruptMessage;
    FTimerHandle InterruptionTimerHandle;
    void OnInterruptionDelayFinished();

    // Delegate trampolines.
    UFUNCTION() void HandleModelLoaded(bool bSuccess, FString ErrorMessage);
    UFUNCTION() void HandleToken(FString RawText, FString CleanText);
    UFUNCTION() void HandleSentence(FString RawText, FString CleanText);
    UFUNCTION() void HandleNewLine();
    UFUNCTION() void HandleComplete(FString FullText);
    UFUNCTION() void HandleError(FString ErrorMessage);
    UFUNCTION() void HandleToolCalled(FName ToolName, FString ArgumentsJson, FString ResultJson);
    UFUNCTION() void HandleAudioReadyToPlay();
    UFUNCTION() void HandleAudioPlaybackFinished();
    UFUNCTION() void HandleDownloadProgress(float Percent, int64 BytesReceived, int64 TotalBytes);

    void CreateConversationAndQueue();
};
