// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsCommon.h"
#include "InoNeuTtsLog.h"
#include "InoNeuTtsSettings.h"

namespace InoNeuTtsNative
{
	FString ResolveGgufPath(EInoNeuTtsVariant Variant, const FString& ModelName)
	{
		const UInoNeuTtsNativeSettings* Settings =
			GetDefault<UInoNeuTtsNativeSettings>();
		if (Settings == nullptr)
		{
			return FString();
		}

		const TArray<FInoNeuTtsBackboneEntry>& Pool =
			(Variant == EInoNeuTtsVariant::Air)
				? Settings->AirModels
				: Settings->NanoModels;

		const FInoNeuTtsBackboneEntry* Entry =
			UInoNeuTtsNativeSettings::FindBackbone(Pool, ModelName);

		if (Entry == nullptr)
		{
			UE_LOG(LogInoNeuTts, Warning,
				TEXT("No backbone entry found for variant %s%s%s. ")
				TEXT("Configure Project Settings -> Ino NeuTTS Native."),
				*VariantToString(Variant),
				ModelName.IsEmpty() ? TEXT("") : TEXT(" / name "),
				ModelName.IsEmpty() ? TEXT("") : *ModelName);
			return FString();
		}

		return UInoNeuTtsNativeSettings::ResolveLocalPath(Entry->LocalFileName);
	}

	FString ResolveOnnxDecoderPath(const FString& ModelName)
	{
		const UInoNeuTtsNativeSettings* Settings =
			GetDefault<UInoNeuTtsNativeSettings>();
		if (Settings == nullptr)
		{
			return FString();
		}

		const FInoNeuTtsDecoderEntry* Entry =
			UInoNeuTtsNativeSettings::FindDecoder(Settings->DecoderModels, ModelName);

		if (Entry == nullptr)
		{
			UE_LOG(LogInoNeuTts, Warning,
				TEXT("No decoder entry found%s%s. ")
				TEXT("Configure Project Settings -> Ino NeuTTS Native."),
				ModelName.IsEmpty() ? TEXT("") : TEXT(" for name "),
				ModelName.IsEmpty() ? TEXT("") : *ModelName);
			return FString();
		}

		return UInoNeuTtsNativeSettings::ResolveLocalPath(Entry->LocalFileName);
	}

	FString VariantToString(EInoNeuTtsVariant Variant)
	{
		switch (Variant)
		{
			case EInoNeuTtsVariant::Nano: return TEXT("Nano");
			case EInoNeuTtsVariant::Air:  return TEXT("Air");
		}
		return TEXT("Unknown");
	}
}
