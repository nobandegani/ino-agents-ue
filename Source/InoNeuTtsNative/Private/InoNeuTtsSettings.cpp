// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsSettings.h"

#include "Misc/Paths.h"

UInoNeuTtsNativeSettings::UInoNeuTtsNativeSettings()
{
	// No defaults — arrays start empty. Configure entries in
	// Project Settings -> Plugins -> InoNeuTtsNative.
}

FString UInoNeuTtsNativeSettings::GetModelsDir()
{
	return FPaths::Combine(
		FPaths::ProjectPersistentDownloadDir(),
		TEXT("InoAgents"),
		TEXT("NeuTTS"));
}

FString UInoNeuTtsNativeSettings::ResolveLocalPath(const FString& LocalFileName)
{
	if (LocalFileName.IsEmpty())
	{
		return FString();
	}
	return FPaths::Combine(GetModelsDir(), LocalFileName);
}

const FInoNeuTtsBackboneEntry* UInoNeuTtsNativeSettings::FindBackbone(
	const TArray<FInoNeuTtsBackboneEntry>& Pool,
	const FString& DesiredName)
{
	if (Pool.Num() == 0)
	{
		return nullptr;
	}

	if (DesiredName.IsEmpty())
	{
		return &Pool[0];
	}

	for (const FInoNeuTtsBackboneEntry& Entry : Pool)
	{
		if (Entry.DisplayName.Equals(DesiredName, ESearchCase::IgnoreCase))
		{
			return &Entry;
		}
	}

	return nullptr;
}

const FInoNeuTtsDecoderEntry* UInoNeuTtsNativeSettings::FindDecoder(
	const TArray<FInoNeuTtsDecoderEntry>& Pool,
	const FString& DesiredName)
{
	if (Pool.Num() == 0)
	{
		return nullptr;
	}

	if (DesiredName.IsEmpty())
	{
		return &Pool[0];
	}

	for (const FInoNeuTtsDecoderEntry& Entry : Pool)
	{
		if (Entry.DisplayName.Equals(DesiredName, ESearchCase::IgnoreCase))
		{
			return &Entry;
		}
	}

	return nullptr;
}
