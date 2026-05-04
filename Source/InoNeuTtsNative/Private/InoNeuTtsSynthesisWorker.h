// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "InoNeuTtsTypes.h"

#include <atomic>

namespace InoNeuTtsNative
{
	class FInoNeuTtsRunner;

	/**
	 * Run one full NeuTTS synthesis end-to-end on the calling thread:
	 *
	 *   1. Phonemize InputText (via InoSpeakNG, language from Voice.Language)
	 *   2. Lazy-phonemize Voice.RefText into Voice.RefPhones if empty
	 *   3. Build the chat-template prompt (PromptBuilder)
	 *   4. Tokenize with parse_special=true, clear KV cache, prefill
	 *   5. Build sampler chain (top-k -> temp -> dist) and AR loop until
	 *      <|SPEECH_GENERATION_END|>, EOG, or MaxNewTokens
	 *   6. Regex-extract <|speech_(\d+)|> ids from generated text
	 *   7. Run NeuCodec ONNX decoder on the int32 codes
	 *   8. Convert float32 [-1,+1] waveform to int16 PCM little-endian
	 *
	 * Slow — call from a worker thread. Not thread-safe with concurrent
	 * RunSynthesis calls on the same Runner (KV cache + sampler state).
	 *
	 * Voice is taken by mutable reference so RefPhones can be cached on
	 * first synth — saves an espeak call on subsequent runs with the
	 * same voice instance. Pass the same FInoNeuTtsVoice across calls
	 * to benefit from the cache.
	 *
	 * CancelFlag is an optional atomic checked every kCancelCheckEvery
	 * AR iterations. Set it to true to abort cleanly with an empty
	 * Result.AudioSamples and ErrorMessage="Cancelled". Pass nullptr to
	 * disable cancellation.
	 */
	FInoNeuTtsResult RunSynthesis(
		FInoNeuTtsRunner& Runner,
		const FString& InputText,
		FInoNeuTtsVoice& Voice,
		const FInoNeuTtsOptions& Options,
		const std::atomic<bool>* CancelFlag = nullptr);
}
