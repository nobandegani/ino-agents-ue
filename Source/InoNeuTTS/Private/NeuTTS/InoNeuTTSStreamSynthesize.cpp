// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "NeuTTS/InoNeuTTSStreamSynthesize.h"

#include "NeuTTS/InoNeuTTSSubsystem.h"

#include "InoAgentsLog.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"

UInoNeuTTSStreamSynthesize* UInoNeuTTSStreamSynthesize::SynthesizeStreamAsync(
    UObject* WorldContextObject,
    const FString& Text,
    const FInoNeuTTSOptions& Options,
    int32 ChunkTokens)
{
    UInoNeuTTSStreamSynthesize* Action = NewObject<UInoNeuTTSStreamSynthesize>();
    Action->WorldContextObject_ = WorldContextObject;
    Action->Text_               = Text;
    Action->Options_            = Options;
    Action->ChunkTokens_        = ChunkTokens;
    return Action;
}

void UInoNeuTTSStreamSynthesize::Activate()
{
    UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(
        WorldContextObject_, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
    UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
    UInoNeuTTSSubsystem* Subsys = GI ? GI->GetSubsystem<UInoNeuTTSSubsystem>() : nullptr;

    if (!Subsys)
    {
        FInoNeuTTSResult R;
        R.ErrorMessage = TEXT("UInoNeuTTSSubsystem not available (no GameInstance / wrong world context)");
        OnError.Broadcast(R);
        SetReadyToDestroy();
        return;
    }

    FInoNeuTTSAudioChunkDelegate ChunkCb;
    ChunkCb.BindDynamic(this, &UInoNeuTTSStreamSynthesize::HandleChunk);

    FInoNeuTTSSynthesisCompleteDelegate DoneCb;
    DoneCb.BindDynamic(this, &UInoNeuTTSStreamSynthesize::HandleComplete);

    Subsys->SynthesizeStreamAsync(Text_, Options_, ChunkTokens_, ChunkCb, DoneCb);
}

void UInoNeuTTSStreamSynthesize::HandleChunk(const TArray<uint8>& AudioChunk, bool bIsFinal)
{
    OnAudioChunk.Broadcast(AudioChunk, bIsFinal);
}

void UInoNeuTTSStreamSynthesize::HandleComplete(const FInoNeuTTSResult& Result)
{
    if (Result.bSuccess) OnComplete.Broadcast(Result);
    else                 OnError.Broadcast(Result);
    SetReadyToDestroy();
}
