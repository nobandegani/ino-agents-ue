// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsPromptBuilder.h"

namespace InoNeuTtsNative
{
	FString BuildSynthesisPrompt(
		const FString& RefPhones,
		const FString& InputPhones,
		const TArray<int32>& RefCodes)
	{
		// Pre-allocate the speech-tokens block: each token is roughly
		// "<|speech_NNNNN|>" — call it 16 chars average. The codec runs at
		// 50 Hz, so this scales with the reference voice length.
		FString SpeechTokens;
		SpeechTokens.Reserve(RefCodes.Num() * 16);
		for (const int32 Code : RefCodes)
		{
			SpeechTokens.Append(FString::Printf(TEXT("<|speech_%d|>"), Code));
		}

		// Single-line, double-quoted to avoid raw-string issues across MSVC
		// versions. The literal "\n" between "<|TEXT_PROMPT_END|>" and
		// "assistant" is intentional — matches Neuphonic's reference.
		return FString::Printf(
			TEXT("user: Convert the text to speech:")
			TEXT("<|TEXT_PROMPT_START|>%s %s<|TEXT_PROMPT_END|>")
			TEXT("\nassistant:<|SPEECH_GENERATION_START|>%s"),
			*RefPhones,
			*InputPhones,
			*SpeechTokens);
	}
}
