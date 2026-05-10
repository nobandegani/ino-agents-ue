// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsSynthesize.h"
#include "InoNeuTtsLog.h"
#include "InoNeuTtsSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"

UInoNeuTtsSynthesize* UInoNeuTtsSynthesize::SynthesizeAsync(
	UObject* WorldContextObject,
	const FString& Text,
	const FInoNeuTtsOptions& Options)
{
	UInoNeuTtsSynthesize* Action = NewObject<UInoNeuTtsSynthesize>();
	Action->WorldContextObjectPtr = WorldContextObject;
	Action->StoredText            = Text;
	Action->StoredOptions         = Options;
	return Action;
}

void UInoNeuTtsSynthesize::Activate()
{
	auto FailFast = [this](const FString& Why)
	{
		FInoNeuTtsResult Bad;
		Bad.bSuccess     = false;
		Bad.ErrorMessage = Why;
		OnError.Broadcast(Bad);
		SetReadyToDestroy();
	};

	UObject* WCO = WorldContextObjectPtr.Get();
	if (WCO == nullptr)
	{
		FailFast(TEXT("WorldContextObject expired before Activate."));
		return;
	}

	UWorld* World = WCO->GetWorld();
	UGameInstance* GI = (World != nullptr) ? World->GetGameInstance() : nullptr;
	UInoNeuTtsSubsystem* Subsystem = (GI != nullptr)
		? GI->GetSubsystem<UInoNeuTtsSubsystem>() : nullptr;

	if (Subsystem == nullptr)
	{
		FailFast(TEXT("UInoNeuTtsSubsystem unavailable from this world / game instance."));
		return;
	}

	FInoNeuTtsSynthesisCompleteDelegate Cb;
	Cb.BindUFunction(this, FName("HandleComplete"));

	Subsystem->SynthesizeAsync(StoredText, StoredOptions, Cb);
}

void UInoNeuTtsSynthesize::HandleComplete(const FInoNeuTtsResult& Result)
{
	if (Result.bSuccess)
	{
		OnComplete.Broadcast(Result);
	}
	else
	{
		OnError.Broadcast(Result);
	}
	SetReadyToDestroy();
}
