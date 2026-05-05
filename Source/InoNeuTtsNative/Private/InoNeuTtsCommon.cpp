// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsCommon.h"
#include "InoNeuTtsLog.h"
#include "InoNeuTtsSettings.h"

namespace InoNeuTtsNative
{
	FString ResolveGgufPath(const FString& ModelName)
	{
		const UInoNeuTtsNativeSettings* Settings =
			GetDefault<UInoNeuTtsNativeSettings>();
		if (Settings == nullptr)
		{
			return FString();
		}

		const FInoNeuTtsBackboneEntry* Entry =
			UInoNeuTtsNativeSettings::FindBackbone(Settings->BackboneModels, ModelName);

		if (Entry == nullptr)
		{
			UE_LOG(LogInoNeuTts, Warning,
				TEXT("No backbone entry found%s%s. ")
				TEXT("Configure Project Settings -> Ino NeuTTS Native -> NeuTTS Backbone."),
				ModelName.IsEmpty() ? TEXT("") : TEXT(" for name "),
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
				TEXT("Configure Project Settings -> Ino NeuTTS Native -> NeuCodec Decoder."),
				ModelName.IsEmpty() ? TEXT("") : TEXT(" for name "),
				ModelName.IsEmpty() ? TEXT("") : *ModelName);
			return FString();
		}

		return UInoNeuTtsNativeSettings::ResolveLocalPath(Entry->LocalFileName);
	}

	bool TokenizePrompt(
		const InoAgents::LlamaCpp::FLlamaCppApi& Api,
		const struct llama_vocab*                Vocab,
		const FString&                           Prompt,
		TArray<llama_token>&                     OutTokens,
		FString&                                 OutError)
	{
		const FTCHARToUTF8 PromptUtf8(*Prompt);
		const int32 ByteLen = PromptUtf8.Length();

		// Probe: a stack buffer big enough for most prompts. If too
		// small, llama_tokenize returns -<required>.
		llama_token Probe[8];
		const int32 N = Api.llama_tokenize(
			Vocab,
			PromptUtf8.Get(), ByteLen,
			Probe, UE_ARRAY_COUNT(Probe),
			/*add_special*/ false,
			/*parse_special*/ true);

		int32 Required = N;
		if (N < 0)
		{
			Required = -N;
		}
		else if (N > 0)
		{
			// Fits in the probe; copy out.
			OutTokens.SetNumUninitialized(N);
			FMemory::Memcpy(OutTokens.GetData(), Probe, sizeof(llama_token) * N);
			return true;
		}
		else
		{
			OutError = TEXT("llama_tokenize produced 0 tokens.");
			return false;
		}

		// Allocate the real buffer and re-tokenize.
		OutTokens.SetNumUninitialized(Required);
		const int32 N2 = Api.llama_tokenize(
			Vocab,
			PromptUtf8.Get(), ByteLen,
			OutTokens.GetData(), OutTokens.Num(),
			/*add_special*/ false,
			/*parse_special*/ true);

		if (N2 != Required)
		{
			OutError = FString::Printf(
				TEXT("llama_tokenize second call returned %d (expected %d)."),
				N2, Required);
			return false;
		}
		return true;
	}
}
