// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "InoNeuTtsTypes.h"

// llama_token + FLlamaCppApi forward decls reachable through InoLlama.h —
// the helpers below take llama types and the API by reference.
#include "InoLlama.h"

namespace InoNeuTtsNative
{
	/**
	 * Resolve the local on-disk path for a NeuTTS backbone (Nano or Air —
	 * the runtime doesn't branch on variant, the GGUF file encodes the size).
	 *
	 * Looks up the matching entry in UInoNeuTtsNativeSettings::BackboneModels.
	 * If ModelName is empty the first entry in the array is used; otherwise
	 * the entry whose DisplayName matches case-insensitively.
	 *
	 * Returns:
	 *   <FPaths::ProjectPersistentDownloadDir()>/InoAgents/NeuTTS/<LocalFileName>
	 * if an entry exists; empty string otherwise.
	 *
	 * Does NOT check whether the file is on disk — callers that depend
	 * on the file (the runner) should ensure it exists or download it
	 * first.
	 */
	FString ResolveGgufPath(const FString& ModelName = FString());

	/**
	 * Resolve the local on-disk path for the NeuCodec ONNX decoder.
	 * Same shape as ResolveGgufPath — looks up DecoderModels in settings.
	 */
	FString ResolveOnnxDecoderPath(const FString& ModelName = FString());

	/**
	 * Tokenize a UTF-8 string through llama.cpp with parse_special=true.
	 * Two-pass: first call probes the required size by passing a small
	 * stack buffer (returns negative count = -required), second call
	 * writes for real.
	 *
	 * `add_special` is forced to false — NeuTTS's chat template already
	 * embeds every special-token string in plain text, so letting the
	 * tokenizer auto-prepend a BOS / system would corrupt the prompt
	 * structure.
	 *
	 * Returns true on success and fills OutTokens. On failure, OutError
	 * is populated and OutTokens is left in an unspecified state.
	 *
	 * Used by both the synth worker (per-call prompt tokenization) and
	 * the runner (voice cache priming).
	 */
	bool TokenizePrompt(
		const InoAgents::LlamaCpp::FLlamaCppApi& Api,
		const struct llama_vocab*                Vocab,
		const FString&                           Prompt,
		TArray<llama_token>&                     OutTokens,
		FString&                                 OutError);

	/**
	 * Normalize whitespace in a phonemized string the same way the
	 * upstream NeuTTS reference's `_to_phones` does:
	 *
	 *     phones = phones.split()
	 *     phones = " ".join(phones)
	 *
	 * I.e. collapse any run of whitespace (spaces, tabs, newlines, CRs)
	 * to a single space and trim edges. eSpeak's TextToPhonemes
	 * occasionally emits leading/trailing whitespace per clause, and
	 * our clause-joining loop in InoSpeakNG can produce double spaces
	 * between clauses — both produce subtly different tokenization
	 * vs the vendor reference if not normalized.
	 */
	FString NormalizePhones(const FString& Phones);
}
