// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"

#include "Audio/InoAgentsAudioTypes.h"
#include "ElevenLabs/ElevenLabsTypes.h"
#include "LiteRtLm/LiteRtLmTypes.h"

#include "LiteRtLmAgentComponent.generated.h"

class UInoAgentsStreamingAudioComponent;
class UInoAgentsLiteRtLmDialogueQueue;
class ULiteRtLmConversation;
class ULiteRtLmSubsystem;

/** Multicast version of FOnLiteRtLmModelLoaded for BlueprintAssignable. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInoAgentsAgentModelLoaded,
    bool, bSuccess,
    FString, ErrorMessage);

/**
 * All-in-one LiteRT-LM + ElevenLabs agent component.
 *
 * Drop this on any actor, configure in the details panel, and call
 * SendMessage. The component handles model loading, conversation
 * creation, TTS dispatch, ordered audio playback, and spatialisation
 * from the actor's 3D position. Everything is internal — no manual
 * wiring of subsystems, queues, or audio components needed.
 *
 * Details panel exposes: model config, voice ID, TTS settings, pause
 * duration, auto-load toggle, and the child Streaming Audio component
 * (with all its inherited UAudioComponent properties like volume,
 * pitch, attenuation, source effect chain, etc.).
 *
 * Blueprint usage:
 *   - Drop "LiteRT-LM Agent" on an actor
 *   - Set ModelConfig (data asset) and VoiceId
 *   - On key press: Agent → Send Message ("Hello")
 *   - Bind OnToken / OnSentence / OnComplete for UI
 *   - Audio plays automatically from the actor's position
 */
UCLASS(ClassGroup = (InoAgents),
       meta = (BlueprintSpawnableComponent, DisplayName = "LiteRT-LM Agent"))
class INOAGENTS_API UInoAgentsLiteRtLmAgentComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UInoAgentsLiteRtLmAgentComponent(const FObjectInitializer& ObjectInitializer);

    // =============================================================
    // Configuration (editable in details panel)
    // =============================================================

    /** Model configuration — set ModelFileName, Backend, SystemMessage
     *  directly in the details panel. No data asset needed. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent")
    FLiteRtLmModelConfig ModelConfig;

    /** ElevenLabs voice ID for TTS. Find yours at
     *  https://elevenlabs.io/app/voice-lab. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent")
    FString VoiceId;

    /** ElevenLabs TTS settings (ModelId, OutputFormat, Stability,
     *  Seed, LanguageCode, ApplyTextNormalization). The Inputs array
     *  is ignored — filled per-sentence internally by the dialogue
     *  queue. Leave fields at their defaults to use the subsystem's
     *  Project Settings values. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent")
    FElevenLabsDialogueRequest TtsRequestTemplate;

    /** Silence in ms inserted between lines. 0 = no pause. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent",
              meta = (ClampMin = "0", ClampMax = "5000"))
    int32 PauseDurationMs = 500;

    /** If true, automatically loads the model and creates a
     *  conversation on BeginPlay. Set to false if you want to call
     *  LoadModel() manually (e.g. after a loading screen). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoAgents|Agent")
    bool bAutoLoadOnBeginPlay = true;

    // =============================================================
    // API
    // =============================================================

    /** Load the model from ModelConfig. Non-blocking. OnModelLoaded
     *  fires when done. If bAutoLoadOnBeginPlay is true, this is
     *  called from BeginPlay automatically. Safe to call multiple
     *  times — subsequent calls after a successful load are no-ops. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Agent")
    void LoadModel();

    /** Send a user message. The model streams a response via
     *  OnToken / OnSentence / OnComplete, and TTS audio plays from
     *  this component's world position. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Agent")
    void SendMessage(const FString& Text);

    /** Cancel the in-flight response. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Agent")
    void Cancel();

    /** Show the debug Slate chat panel connected to this agent's
     *  conversation. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Agent")
    void ShowChatPanel();

    /** Hide the debug chat panel. */
    UFUNCTION(BlueprintCallable, Category = "InoAgents|Agent")
    void HideChatPanel();

    // =============================================================
    // Delegates (pass-through from internal conversation + queue)
    // =============================================================

    /** Fires when LoadModel completes (success or failure). */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoAgentsAgentModelLoaded OnModelLoaded;

    /** Fires per streaming token from the model. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnLiteRtLmToken OnToken;

    /** Fires per line (newline-delimited). RawText has [emotion] tags
     *  for TTS; CleanText has them stripped for UI display. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnLiteRtLmSentence OnSentence;

    /** Fires once when the model finishes its full response. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnLiteRtLmComplete OnComplete;

    /** Fires once on any error (model error, TTS error, etc.). */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnLiteRtLmError OnError;

    /** Fires when all queued TTS audio has finished playing. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoAgentsAudioFinished OnAudioFinished;

    /** Fires during model download with progress info. Use for
     *  loading screens / progress bars. Only fires when the model
     *  isn't cached locally and needs to be downloaded. */
    UPROPERTY(BlueprintAssignable, Category = "InoAgents|Agent")
    FOnInoAgentsModelDownloadProgress OnDownloadProgress;

    // =============================================================
    // Read-only state
    // =============================================================

    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    bool IsModelLoaded() const;

    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    bool IsStreaming() const;

    /** Access the internal conversation for advanced delegate binding
     *  (e.g. OnToolCalled). */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    ULiteRtLmConversation* GetConversation() const { return Conversation; }

    /** Access the child audio component for volume / pitch /
     *  attenuation tweaks at runtime. Also visible as a child in the
     *  details panel. */
    UFUNCTION(BlueprintPure, Category = "InoAgents|Agent")
    UInoAgentsStreamingAudioComponent* GetAudioComponent() const { return AudioComp; }

    //~ USceneComponent interface
    virtual void BeginPlay() override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
    //~ End USceneComponent interface

private:
    /** Child audio component — created in constructor so it appears
     *  in the details panel with its inherited UAudioComponent
     *  properties (volume, pitch, attenuation, etc.). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "InoAgents|Agent",
              meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UInoAgentsStreamingAudioComponent> AudioComp;

    UPROPERTY()
    TObjectPtr<ULiteRtLmConversation> Conversation;

    UPROPERTY()
    TObjectPtr<UInoAgentsLiteRtLmDialogueQueue> DialogueQueue;

    TWeakObjectPtr<ULiteRtLmSubsystem> SubsystemWeak;

    // Delegate trampolines — UFUNCTION required for dynamic delegates.
    UFUNCTION() void HandleModelLoaded(bool bSuccess, FString ErrorMessage);
    UFUNCTION() void HandleToken(FString Chunk);
    UFUNCTION() void HandleSentence(FString RawText, FString CleanText);
    UFUNCTION() void HandleComplete(FString FullText);
    UFUNCTION() void HandleError(FString ErrorMessage);
    UFUNCTION() void HandleAudioFinished();

    void CreateConversationAndQueue();

    // Download progress trampoline from subsystem → component delegate.
    UFUNCTION()
    void HandleDownloadProgress(float Percent, int64 BytesReceived, int64 TotalBytes);
};
