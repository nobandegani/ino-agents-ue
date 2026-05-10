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
		// Mirror BuildSynthesisPrompt's prefix EXCEPT for the trailing
		// space. Qwen2's BPE is byte-level + GPT-2-style space-prefix
		// pre-tokenization: a trailing whitespace tokenizes as its own
		// [" "] piece in standalone, but the equivalent character
		// position in the full prompt sees the space merged with the
		// next word into a single ` X` token. That makes the LAST token
		// of the standalone prefix differ from the per-call full-prompt
		// tokenization at the equivalent position, causing the synth-
		// time Memcmp guard to fail and forcing a full prefill every
		// call.
		//
		// Ending the prefix at the last RefPhones character lands inside
		// the previous word, where BPE is stable across contexts (next-
		// char doesn't affect already-emitted tokens once you've crossed
		// a word boundary). The synth path then prefills " {InputPhones}
		// ...{TEXT_PROMPT_END}\nassistant:..." as the suffix — same KV
		// state as a full prefill, just with a clean cache hit.
		return FString::Printf(
			TEXT("user: Convert the text to speech:")
			TEXT("<|TEXT_PROMPT_START|>%s"),
			*RefPhones);
	}
}
