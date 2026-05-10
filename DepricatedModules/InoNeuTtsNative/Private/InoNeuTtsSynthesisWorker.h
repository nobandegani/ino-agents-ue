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
	 *   2. Phonemize Voice.RefText (or use Voice.RefPhones if pre-baked)
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
	 * To skip the runtime ref_text phonemization on every synth, callers
	 * can pre-bake Voice.RefPhones once and reuse the same voice
	 * instance (the build-voices.py offline path leaves RefPhones empty;
	 * fill it in callers if you want to amortize the cost).
	 *
	 * CancelFlag is an optional atomic checked every kCancelCheckEvery
	 * AR iterations. Set it to true to abort cleanly with an empty
	 * Result.AudioSamples and ErrorMessage="Cancelled". Pass nullptr to
	 * disable cancellation.
	 */
	FInoNeuTtsResult RunSynthesis(
		FInoNeuTtsRunner& Runner,
		const FString& InputText,
		const FInoNeuTtsVoice& Voice,
		const FInoNeuTtsOptions& Options,
		const std::atomic<bool>* CancelFlag = nullptr);

	/**
	 * Callbacks fired from the worker thread during streaming synthesis.
	 * The subsystem marshals these to the game thread before invoking
	 * Blueprint delegates. OnChunk is invoked once per emitted chunk;
	 * the final invocation has bIsFinal=true.
	 */
	struct FInoNeuTtsStreamCallbacks
	{
		TFunction<void(TArray<uint8> ChunkBytes, bool bIsFinal)> OnChunk;
	};

	/**
	 * Streaming variant of RunSynthesis. Same end-state — returns a
	 * FInoNeuTtsResult with the concatenated full waveform. The
	 * difference is that, as the AR loop progresses, audio is decoded
	 * and emitted in overlap-added chunks of `ChunkTokens` codec frames
	 * each via Callbacks.OnChunk.
	 *
	 * Algorithm follows neutts.py `_infer_stream_ggml` + `_linear_overlap_add`:
	 *   - Each chunk decodes a window of `ChunkTokens + lookforward +
	 *     overlap` new tokens plus `lookback + overlap` of context.
	 *   - The middle portion of the resulting waveform (cropping out
	 *     lookback / lookforward) is added to a running list.
	 *   - Linear overlap-add over the running list smooths boundaries
	 *     between adjacent chunks.
	 *   - The newly-stable samples (those past the previous emit point
	 *     up to the current `len(audio_cache) * stride_samples`) are
	 *     emitted via OnChunk.
	 *   - After the AR loop ends, a final irregular chunk handles the
	 *     remaining tokens with bIsFinal=true.
	 *
	 * `ChunkTokens` = 25 matches the Python default (~0.5 s @ 24 kHz).
	 * Smaller values reduce first-audio latency at the cost of more
	 * decoder runs. Pass <= 0 to fall back to one-shot semantics
	 * (single OnChunk with bIsFinal=true at the end).
	 */
	FInoNeuTtsResult RunStreamingSynthesis(
		FInoNeuTtsRunner& Runner,
		const FString& InputText,
		const FInoNeuTtsVoice& Voice,
		const FInoNeuTtsOptions& Options,
		int32 ChunkTokens,
		const FInoNeuTtsStreamCallbacks& Callbacks,
		const std::atomic<bool>* CancelFlag = nullptr);
}
