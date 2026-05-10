// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Qwen3ASR/InoQwen3ASRLiteRTTranscribeAction.h"

#include "Qwen3ASR/InoQwen3ASRLiteRTSubsystem.h"
#include "InoQwen3ASRLiteRT.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"

UInoQwen3ASRLiteRTTranscribeAction*
UInoQwen3ASRLiteRTTranscribeAction::TranscribeWavFile(
    UObject* WorldContextObject,
    const FString& WavPath)
{
    UInoQwen3ASRLiteRTTranscribeAction* Action = NewObject<UInoQwen3ASRLiteRTTranscribeAction>();
    Action->WorldContextObject = WorldContextObject;
    Action->WavPath = WavPath;
    return Action;
}

void UInoQwen3ASRLiteRTTranscribeAction::Activate()
{
    if (!WorldContextObject)
    {
        OnError.Broadcast(TEXT("Internal: missing WorldContextObject"));
        SetReadyToDestroy();
        return;
    }
    UWorld* World = WorldContextObject->GetWorld();
    UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
    UInoQwen3ASRLiteRTSubsystem* Subsystem = GameInstance
        ? GameInstance->GetSubsystem<UInoQwen3ASRLiteRTSubsystem>()
        : nullptr;
    if (!Subsystem)
    {
        OnError.Broadcast(TEXT("UInoQwen3ASRLiteRTSubsystem unavailable (no GameInstance)."));
        SetReadyToDestroy();
        return;
    }
    if (!Subsystem->IsModelLoaded())
    {
        OnError.Broadcast(TEXT("Model not loaded — call Load Model (Qwen3 ASR) first."));
        SetReadyToDestroy();
        return;
    }

    FOnInoQwen3ASRTranscribeComplete Cb;
    Cb.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(
        UInoQwen3ASRLiteRTTranscribeAction, HandleComplete));
    Subsystem->TranscribeWavFileAsync(WavPath, Cb);
}

void UInoQwen3ASRLiteRTTranscribeAction::HandleComplete(
    bool bSuccess,
    FInoQwen3ASRTranscribeResult Result,
    FString ErrorMessage)
{
    if (bSuccess)
    {
        OnComplete.Broadcast(Result);
    }
    else
    {
        OnError.Broadcast(ErrorMessage);
    }
    SetReadyToDestroy();
}
