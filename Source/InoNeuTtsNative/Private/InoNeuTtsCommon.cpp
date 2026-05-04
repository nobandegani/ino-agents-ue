// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsCommon.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace InoNeuTtsNative
{
	namespace
	{
		FString GetNeuTtsModelsDir()
		{
			const TSharedPtr<IPlugin> Plugin =
				IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
			if (!Plugin.IsValid())
			{
				return FString();
			}
			return FPaths::Combine(Plugin->GetBaseDir(), TEXT("NeuTTS"), TEXT("models"));
		}
	}

	FString ResolveGgufPath(EInoNeuTtsVariant Variant)
	{
		const FString Models = GetNeuTtsModelsDir();
		if (Models.IsEmpty())
		{
			return FString();
		}

		switch (Variant)
		{
			case EInoNeuTtsVariant::Nano:
				return FPaths::Combine(Models,
					TEXT("nano-q4-gguf"), TEXT("neutts-nano-Q4_0.gguf"));

			case EInoNeuTtsVariant::Air:
				return FPaths::Combine(Models,
					TEXT("air-q4-gguf"), TEXT("neutts-air-Q4_0.gguf"));
		}
		return FString();
	}

	FString ResolveOnnxDecoderPath()
	{
		const FString Models = GetNeuTtsModelsDir();
		if (Models.IsEmpty())
		{
			return FString();
		}
		return FPaths::Combine(Models,
			TEXT("onnx-decoder-int8"), TEXT("model.onnx"));
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
