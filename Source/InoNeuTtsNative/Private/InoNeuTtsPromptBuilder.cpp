// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsPromptBuilder.h"

namespace InoNeuTtsNative
{
	FString BuildSpeechTokensBlock(const TArray<int32>& RefCodes)
	{
		// Pre-allocate roughly 16 chars per "<|speech_NNNNN|>" token. The
		// codec runs at 50 Hz, so this scales with the reference voice
		// length (typical: ~650 codes for a 13 s reference).
		FString SpeechTokens;
		SpeechTokens.Reserve(RefCodes.Num() * 16);
		for (const int32 Code : RefCodes)
		{
			SpeechTokens.Append(FString::Printf(TEXT("<|speech_%d|>"), Code));
		}
		return SpeechTokens;
	}

	FString BuildSynthesisPrompt(
		const FString& RefPhones,
		const FString& InputPhones,
		const FString& SpeechTokensBlock)
	{
		// Single-line, double-quoted to avoid raw-string issues across MSVC
		// versions. The literal "\n" between "<|TEXT_PROMPT_END|>" and
		// "assistant" is intentional — matches Neuphonic's reference.
		return FString::Printf(
			TEXT("user: Convert the text to speech:")
			TEXT("<|TEXT_PROMPT_START|>%s %s<|TEXT_PROMPT_END|>")
			TEXT("\nassistant:<|SPEECH_GENERATION_START|>%s"),
			*RefPhones,
			*InputPhones,
			*SpeechTokensBlock);
	}

	FString BuildSynthesisPrompt(
		const FString& RefPhones,
		const FString& InputPhones,
		const TArray<int32>& RefCodes)
	{
		// Slow path: builds the speech-tokens block fresh from RefCodes.
		// Voice-cache hits go through the FString-overload above and skip
		// this 650-Printf cost entirely.
		const FString SpeechTokens = BuildSpeechTokensBlock(RefCodes);
		return BuildSynthesisPrompt(RefPhones, InputPhones, SpeechTokens);
	}

	FString BuildSynthesisPromptPrefix(const FString& RefPhones)
	{
		// Mirror BuildSynthesisPrompt's prefix exactly — the format must
		// be byte-identical so the per-call full prompt's leading tokens
		// match the cached prefix's tokens. The trailing space is the
		// separator between {ref_phones} and {input_phones}; including
		// it here means InputPhones tokenization starts at a fresh word
		// boundary at synth time, matching how the full-prompt
		// tokenization splits the same content.
		return FString::Printf(
			TEXT("user: Convert the text to speech:")
			TEXT("<|TEXT_PROMPT_START|>%s "),
			*RefPhones);
	}
}
