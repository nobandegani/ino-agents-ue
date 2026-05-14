// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "NeuTTS/InoNeuTTSSynthesize.h"

#include "NeuTTS/InoNeuTTSSubsystem.h"

#include "InoAgentsLog.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"

UInoNeuTTSSynthesize* UInoNeuTTSSynthesize::SynthesizeAsync(
    UObject* WorldContextObject,
    const FString& Text,
    const FInoNeuTTSOptions& Options)
{
    UInoNeuTTSSynthesize* Action = NewObject<UInoNeuTTSSynthesize>();
    Action->WorldContextObject_ = WorldContextObject;
    Action->Text_               = Text;
    Action->Options_            = Options;
    return Action;
}

void UInoNeuTTSSynthesize::Activate()
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

    FInoNeuTTSSynthesisCompleteDelegate Cb;
    Cb.BindDynamic(this, &UInoNeuTTSSynthesize::HandleComplete);
    Subsys->SynthesizeAsync(Text_, Options_, Cb);
}

void UInoNeuTTSSynthesize::HandleComplete(const FInoNeuTTSResult& Result)
{
    if (Result.bSuccess) OnComplete.Broadcast(Result);
    else                 OnError.Broadcast(Result);
    SetReadyToDestroy();
}
