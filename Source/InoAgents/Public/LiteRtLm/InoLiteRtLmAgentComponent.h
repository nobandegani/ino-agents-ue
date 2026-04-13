// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "Audio/InoAudioTypes.h"
#include "ElevenLabs/InoElevenLabsTypes.h"
#include "LiteRtLm/InoLiteRtLmConversation.h"   // EInoLiteRtLmSentenceSplit
#include "LiteRtLm/InoLiteRtLmTypes.h"

#include "InoLiteRtLmAgentComponent.generated.h"

class UInoStreamingSoundWave;
class UInoLiteRtLmDialogueQueue;
class UInoLiteRtLmConversation;
class UInoLiteRtLmSubsystem;
class UInoLiteRtLmToolBase;

/** Agent emotional state. Driven by the set_emotion tool. */
UENUM(BlueprintType)
enum class EInoEmotion : uint8
{
    Neutral     UMETA(DisplayName = "Neutral"),
    Happy       UMETA(DisplayName = "Happy"),
    Sad         UMETA(DisplayName = "Sad"),
    Disgust     UMETA(DisplayName = "Disgust"),
    Anger       UMETA(DisplayName = "Anger"),
    Surprise    UMETA(DisplayName = "Surprise"),
    Fear        UMETA(DisplayName = "Fear"),
    Confident   UMETA(DisplayName = "Confident"),
    Excited     UMETA(DisplayName = "Excited"),
    Bored       UMETA(DisplayName = "Bored"),
    Playful     UMETA(DisplayName = "Playful"),
    Confused    UMETA(DisplayName = "Confused"),
};

/** Agent activity state. */
UENUM(BlueprintType)
enum class EInoAgentStatus : uint8
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

/** Multicast version of FOnInoLiteRtLmModelLoaded for BlueprintAssignable. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInoAgentModelLoaded,
    bool, bSuccess,
    FString, ErrorMessage);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInoAgentStatusChanged,
    EInoAgentStatus, NewStatus);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInoEmotionChanged,
    EInoEmotion, NewEmotion);

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
class INOAGENTS_API UInoLiteRtLmAgentComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UInoLiteRtLmAgentComponent(const FObjectInitializer& ObjectInitializer);

    // =============================================================
    // Configuration (editable in details panel as defaults)
    // =============================================================

    /** Model configuration — ModelFileName, Backend, SystemMessage. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent")
    FInoLiteRtLmModelConfig ModelConfig;

    /** ElevenLabs voice ID. Default: EwVlpfIFmNJ50rqcxXfJ. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent")
    FString VoiceId = TEXT("EwVlpfIFmNJ50rqcxXfJ");

    /** ElevenLabs TTS settings. Default: model=eleven_v3, format=Pcm_16000. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent")
    FInoElevenLabsDialogueRequest TtsRequestTemplate;

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

    /** PCM sample rate applied to the streaming wave. Must match the
     *  ElevenLabs output format (e.g. 16000 for Pcm_16000). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent|Audio",
              meta = (ClampMin = "8000", ClampMax = "192000"))
    int32 PcmSampleRate = 16000;

    /** PCM channel count applied to the streaming wave. 1 = mono, 2 = stereo. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent|Audio",
              meta = (ClampMin = "1", ClampMax = "2"))
    int32 PcmNumChannels = 1;

    /** Number of interleaved float samples per OnGeneratePCMData fire
     *  on the underlying streaming wave. 160 = 10 ms at 16 kHz mono,
     *  a standard cadence for lip-sync / viseme systems. Set to 0 to
     *  disable batching (one broadcast per audio-engine poll). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent|Audio",
              meta = (ClampMin = "0", ClampMax = "16384"))
    int32 NumSamplesPerChunk = 160;

    /** Bitmask of boundaries that trigger OnSentence on the underlying
     *  conversation. Applied at conversation creation AND whenever the
     *  property is changed at runtime (Initialize re-sends it). See
     *  EInoLiteRtLmSentenceSplit. Default matches the conversation's own
     *  default: Newline | Period | Comma | Question | Exclamation. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent",
              meta = (Bitmask, BitmaskEnum = "/Script/InoAgents.EInoLiteRtLmSentenceSplit"))
    int32 SentenceSplitFlags =
          static_cast<int32>(EInoLiteRtLmSentenceSplit::Newline)
        | static_cast<int32>(EInoLiteRtLmSentenceSplit::Period)
        | static_cast<int32>(EInoLiteRtLmSentenceSplit::Comma)
        | static_cast<int32>(EInoLiteRtLmSentenceSplit::Question)
        | static_cast<int32>(EInoLiteRtLmSentenceSplit::Exclamation);

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
        const FInoLiteRtLmModelConfig& InModelConfig,
        const FString& InVoiceId,
        const FInoElevenLabsDialogueRequest& InTtsRequestTemplate,
        int32 InPauseDurationMs,
        float InInterruptionDelaySec,
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
    FOnInoAgentModelLoaded OnModelLoaded;

    /** Fires per streaming token from the model. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoLiteRtLmToken OnToken;

    /** Fires per newline-delimited line. RawText has [emotion] tags;
     *  CleanText has them stripped for UI. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoLiteRtLmSentence OnSentence;

    /** Fires at each newline boundary (after OnSentence). */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoLiteRtLmNewLine OnNewLine;

    /** Fires once when the model finishes its full response. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoLiteRtLmComplete OnComplete;

    /** Fires once on any error. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoLiteRtLmError OnError;

    /** Fires once per tool call executed (diagnostic). */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoLiteRtLmToolCalled OnToolCalled;

    // =============================================================
    // Delegates — dialogue queue / audio
    // =============================================================

    /** Fires when all queued TTS audio has finished playing. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoAudioPlaybackFinished OnAudioFinished;

    /** Fires during model download. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoModelDownloadProgress OnDownloadProgress;

    // =============================================================
    // Delegates — status & emotion
    // =============================================================

    /** Fires whenever the agent's status changes. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoAgentStatusChanged OnStatusChanged;

    /** Fires whenever the agent's emotion changes (driven by the
     *  set_emotion tool during conversation). */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoEmotionChanged OnEmotionChanged;

    // =============================================================
    // Read-only state
    // =============================================================

    /** Current agent status. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    EInoAgentStatus GetStatus() const { return Status; }

    /** Current agent emotion. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    EInoEmotion GetEmotion() const { return Emotion; }

    /** Set emotion and broadcast OnEmotionChanged. Called by the
     *  set_emotion tool — can also be called directly from Blueprint. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Agent")
    void SetEmotion(EInoEmotion NewEmotion);

    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    bool IsModelLoaded() const;

    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    bool IsStreaming() const;

    /** Access the internal conversation for advanced use. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    UInoLiteRtLmConversation* GetConversation() const { return Conversation; }

    /**
     * Access the streaming sound wave that receives TTS audio from the
     * dialogue queue. The agent does NOT create a UAudioComponent for
     * this wave — Blueprint is responsible for:
     *   1. Getting the wave via this accessor
     *   2. Setting it on a UAudioComponent (SetSound)
     *   3. Calling Play() on the audio component when ready
     *   4. Calling Stop() on interruption (bind to OnStatusChanged
     *      → Interrupted) if instant silence is desired
     *
     * Bind to the wave's delegates directly:
     *   OnGeneratePCMData       — playback visualization / lip-sync
     *   OnPopulateAudioData     — incoming TTS data analysis
     *   OnAudioPlaybackFinished — "audio actually ended" signal (only
     *                             fires if BP is playing the wave)
     */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    UInoStreamingSoundWave* GetStreamingSoundWave() const { return StreamingWave; }

    /** Access the internal dialogue queue. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    UInoLiteRtLmDialogueQueue* GetDialogueQueue() const { return DialogueQueue; }

    //~ UActorComponent interface
    virtual void BeginPlay() override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
    //~ End UActorComponent interface

private:
    UPROPERTY()
    TObjectPtr<UInoStreamingSoundWave> StreamingWave;

    UPROPERTY()
    TObjectPtr<UInoLiteRtLmConversation> Conversation;

    UPROPERTY()
    TObjectPtr<UInoLiteRtLmDialogueQueue> DialogueQueue;

    TWeakObjectPtr<UInoLiteRtLmSubsystem> SubsystemWeak;

    EInoAgentStatus Status = EInoAgentStatus::Idle;
    EInoEmotion Emotion = EInoEmotion::Neutral;

    /** Set status and broadcast OnStatusChanged if it actually changed. */
    void SetStatus(EInoAgentStatus NewStatus);


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
    UFUNCTION() void HandleWavePopulateAudioData(const TArray<float>& PopulatedAudioData);
    UFUNCTION() void HandleAudioPlaybackFinished();
    UFUNCTION() void HandleDownloadProgress(float Percent, int64 BytesReceived, int64 TotalBytes);

    /** Latched so the first "data appeared on the wave" trip after a
     *  SendMessage is what transitions us from Thinking to Talking.
     *  Cleared on SendMessage + after OnAudioPlaybackFinished so the
     *  next cycle sees it fresh. */
    bool bTalkingLatched = false;

    void CreateConversationAndQueue();
};
