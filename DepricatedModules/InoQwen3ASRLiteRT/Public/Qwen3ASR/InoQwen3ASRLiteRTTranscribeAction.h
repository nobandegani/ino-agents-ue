// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Qwen3ASR/InoQwen3ASRLiteRTTypes.h"
#include "InoQwen3ASRLiteRTTranscribeAction.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FInoQwen3ASRTranscribeBPDelegate,
    FInoQwen3ASRTranscribeResult, Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FInoQwen3ASRTranscribeErrorBPDelegate,
    FString, ErrorMessage);

/**
 * Latent Blueprint node "Transcribe WAV File (Qwen3 ASR)".
 *
 * Usage:
 *   1. Drag from a `WavPath` (FString) into the node.
 *   2. The node fires OnComplete with the FInoQwen3ASRTranscribeResult on
 *      successful transcription, or OnError with a diagnostic string on
 *      failure (model not loaded, WAV bad format, etc.).
 *
 * Implementation: forwards to UInoQwen3ASRLiteRTSubsystem under the hood;
 * the action exists only so the Blueprint surface looks like a native
 * async UE node.
 */
UCLASS()
class INOQWEN3ASRLITERT_API UInoQwen3ASRLiteRTTranscribeAction
    : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable)
    FInoQwen3ASRTranscribeBPDelegate OnComplete;

    UPROPERTY(BlueprintAssignable)
    FInoQwen3ASRTranscribeErrorBPDelegate OnError;

    /**
     * Transcribe a WAV file (16 kHz mono int16/float32 PCM). The
     * subsystem must already have been loaded via Load Model (Qwen3 ASR);
     * if it isn't, OnError fires immediately.
     */
    UFUNCTION(BlueprintCallable, Category = "Qwen3 ASR",
        meta = (BlueprintInternalUseOnly = "true",
                WorldContext = "WorldContextObject",
                DisplayName = "Transcribe WAV File (Qwen3 ASR)"))
    static UInoQwen3ASRLiteRTTranscribeAction* TranscribeWavFile(
        UObject* WorldContextObject,
        const FString& WavPath);

    virtual void Activate() override;

private:
    UPROPERTY()
    TObjectPtr<UObject> WorldContextObject;

    FString WavPath;

    UFUNCTION()
    void HandleComplete(bool bSuccess, FInoQwen3ASRTranscribeResult Result, FString ErrorMessage);
};
