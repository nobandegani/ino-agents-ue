// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

namespace InoNeuTtsNative
{
	/**
	 * Build the chat-template prompt string for a single NeuTTS synthesis,
	 * matching Neuphonic's reference Python `_infer_ggml`:
	 *
	 *     user: Convert the text to speech:<|TEXT_PROMPT_START|>
	 *     {ref_phones} {input_phones}
	 *     <|TEXT_PROMPT_END|>
	 *     assistant:<|SPEECH_GENERATION_START|>
	 *     <|speech_N1|><|speech_N2|>...<|speech_Nk|>
	 *
	 * (Newlines added above for readability; the produced string is on
	 * a single line except for the literal "\n" before "assistant:".)
	 *
	 * The trailing speech tokens are the reference voice's pre-encoded
	 * codes; the LLM continues by generating MORE <|speech_N|> tokens
	 * until it emits <|SPEECH_GENERATION_END|>.
	 */
	FString BuildSynthesisPrompt(
		const FString& RefPhones,
		const FString& InputPhones,
		const TArray<int32>& RefCodes);
}
