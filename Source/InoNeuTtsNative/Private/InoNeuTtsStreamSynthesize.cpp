// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsStreamSynthesize.h"
#include "InoNeuTtsLog.h"
#include "InoNeuTtsSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"

UInoNeuTtsStreamSynthesize* UInoNeuTtsStreamSynthesize::SynthesizeStreamAsync(
	UObject* WorldContextObject,
	const FString& Text,
	const FInoNeuTtsOptions& Options,
	int32 ChunkTokens)
{
	UInoNeuTtsStreamSynthesize* Action = NewObject<UInoNeuTtsStreamSynthesize>();
	Action->WorldContextObjectPtr = WorldContextObject;
	Action->StoredText            = Text;
	Action->StoredOptions         = Options;
	Action->StoredChunkTokens     = ChunkTokens;
	return Action;
}

void UInoNeuTtsStreamSynthesize::Activate()
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
		FailFast(TEXT("UInoNeuTtsSubsystem unavailable."));
		return;
	}

	FInoNeuTtsAudioChunkDelegate ChunkCb;
	ChunkCb.BindUFunction(this, FName("HandleChunk"));

	FInoNeuTtsSynthesisCompleteDelegate DoneCb;
	DoneCb.BindUFunction(this, FName("HandleComplete"));

	Subsystem->SynthesizeStreamAsync(
		StoredText, StoredOptions,
		StoredChunkTokens, ChunkCb, DoneCb);
}

void UInoNeuTtsStreamSynthesize::HandleChunk(
	const TArray<uint8>& ChunkBytes, bool bIsFinal)
{
	OnAudioChunk.Broadcast(ChunkBytes, bIsFinal);
}

void UInoNeuTtsStreamSynthesize::HandleComplete(const FInoNeuTtsResult& Result)
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
