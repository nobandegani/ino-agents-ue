// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "InoNeuTtsTypes.h"
#include "Templates/UniquePtr.h"

// Pulled in by InoLlama.h transitively, but include directly so this
// header is self-contained for callers.
#include "InoLlama.h"

class FInoOnnxSession;

namespace InoNeuTtsNative
{
	/**
	 * Cached state for a primed voice. Built once by FInoNeuTtsRunner::PrimeVoice
	 * and reused on every synth call until the active voice changes.
	 *
	 * What's cached:
	 *   - VoiceName              — identifies which voice the snapshot is for.
	 *   - ResolvedRefPhones      — the IPA phonemization of Voice.RefText,
	 *                              already whitespace-normalized. Lazy-
	 *                              computed at prime time so synth calls
	 *                              don't re-phonemize / re-normalize the
	 *                              reference text.
	 *   - SpeechTokensBlock      — pre-built `<|speech_N1|><|speech_N2|>...`
	 *                              string (~10 KB for a typical 650-token
	 *                              voice). Built once at prime time so synth
	 *                              calls don't re-run 650 FString::Printf
	 *                              + concatenations every time (this was a
	 *                              dominant per-synth cost before caching).
	 *   - PrefixTokens           — pre-tokenized form of the cacheable prompt
	 *                              prefix (the leading text + RefPhones, but
	 *                              NOT the trailing space before InputPhones).
	 *                              Ending at the last RefPhones character
	 *                              keeps tokenization stable across contexts
	 *                              (Qwen2's byte-level BPE merges trailing
	 *                              whitespace into the next word, so a
	 *                              standalone-tokenized trailing space would
	 *                              not match the equivalent position in the
	 *                              full-prompt tokenization). Stored so synth
	 *                              calls can cross-check the per-call full-
	 *                              prompt tokenization matches the cache
	 *                              before relying on the snapshot.
	 *   - KvSnapshot             — output of llama_state_seq_get_data after
	 *                              prefilling PrefixTokens. Restored at the
	 *                              start of every synth via _set_data, then
	 *                              the remaining (variable-middle + suffix)
	 *                              tokens are appended via one llama_decode.
	 *
	 * Threading: all reads/writes on the worker thread. Snapshot bytes are
	 * a flat byte array (not a TArray of UObjects), so move/copy is cheap
	 * and there are no UPROPERTY or GC concerns.
	 */
	struct FInoNeuTtsVoiceCache
	{
		FString             VoiceName;
		FString             ResolvedRefPhones;
		FString             SpeechTokensBlock;
		TArray<llama_token> PrefixTokens;
		TArray<uint8>       KvSnapshot;

		bool IsValid() const
		{
			return !VoiceName.IsEmpty()
				&& KvSnapshot.Num() > 0
				&& PrefixTokens.Num() > 0
				&& !SpeechTokensBlock.IsEmpty();
		}
	};

	/**
	 * RAII owner of the loaded NeuTTS engine state. Heap-allocated via
	 * TUniquePtr; non-copyable, non-movable (use the unique_ptr to move
	 * ownership). One Runner = one set of model resources, used for as
	 * many synth calls as you like.
	 *
	 * Holds:
	 *   - llama_model* + llama_context* + cached llama_vocab* (the GGUF
	 *     backbone, loaded via the InoLlama-supplied FLlamaCppApi vtable)
	 *   - Cached llama_token StopTokenId (id of "<|SPEECH_GENERATION_END|>",
	 *     resolved once at load via single-token tokenization)
	 *   - TUniquePtr<FInoOnnxSession> for the NeuCodec ONNX decoder
	 *   - Optional FInoNeuTtsVoiceCache for the active primed voice (set
	 *     by PrimeVoice; consumed by RunSynthesis / RunStreamingSynthesis
	 *     when the per-call Voice matches the cache's VoiceName).
	 *
	 * Threading:
	 *   Construct off the game thread (it's slow — multi-GB GGUF mmap +
	 *   ONNX session creation). Once constructed, the worker thread owns
	 *   it for synth runs. Destruction is safe from any thread because
	 *   llama.cpp + ORT free routines have no thread-affinity beyond
	 *   "no concurrent users".
	 */
	class FInoNeuTtsRunner
	{
	public:
		~FInoNeuTtsRunner();

		FInoNeuTtsRunner(const FInoNeuTtsRunner&)            = delete;
		FInoNeuTtsRunner& operator=(const FInoNeuTtsRunner&) = delete;
		FInoNeuTtsRunner(FInoNeuTtsRunner&&)                 = delete;
		FInoNeuTtsRunner& operator=(FInoNeuTtsRunner&&)      = delete;

		/**
		 * Load the GGUF backbone + the ONNX decoder. Returns nullptr on
		 * any failure (with a diagnostic in OutError and a corresponding
		 * Log line). Slow — call from a worker thread.
		 *
		 * GgufPath / OnnxDecoderPath are absolute filesystem paths;
		 * resolve via InoNeuTtsCommon helpers.
		 */
		static TUniquePtr<FInoNeuTtsRunner> Create(
			const FInoNeuTtsConfig& Config,
			const FString& GgufPath,
			const FString& OnnxDecoderPath,
			FString& OutError);

		// ---- Accessors (used by worker / smoke tests) ----

		const InoAgents::LlamaCpp::FLlamaCppApi& GetLlamaApi() const { return *LlamaApi; }
		struct llama_model*                      GetModel()    const { return Model;    }
		struct llama_context*                    GetContext()  const { return Context;  }
		const struct llama_vocab*                GetVocab()    const { return Vocab;    }
		llama_token                              GetStopTokenId() const { return StopTokenId; }
		FInoOnnxSession*                         GetDecoder()  const { return Decoder.Get(); }
		const FString&                           GetModelDescription() const { return ModelDesc; }

		// ---- Voice cache (KV snapshot of the prompt prefix) ----

		/**
		 * Prime the voice cache: phonemize Voice.RefText (if not already
		 * pre-baked into Voice.RefPhones), tokenize the cacheable prefix
		 * + speech codes, prefill the prefix into the LM context, snapshot
		 * the KV state.
		 *
		 * After this returns true, the next RunSynthesis / RunStreamingSynthesis
		 * call with the same Voice (matched by Voice.Name) will skip
		 * tokenizing + prefilling the prefix and reuse the snapshot.
		 *
		 * Slow — call from the worker thread. Replaces any existing cache.
		 */
		bool PrimeVoice(const FInoNeuTtsVoice& Voice, FString& OutError);

		/** Drop any primed voice cache. Cheap; no LM state changes. */
		void ClearVoiceCache();

		/** True iff a primed voice with the given name is loaded. */
		bool HasCachedVoice(const FString& VoiceName) const;

		/** Read-only access to the cache (nullptr if nothing primed). */
		const FInoNeuTtsVoiceCache* GetVoiceCache() const
		{
			return VoiceCache.IsValid() ? VoiceCache.Get() : nullptr;
		}

	private:
		FInoNeuTtsRunner() = default;

		const InoAgents::LlamaCpp::FLlamaCppApi* LlamaApi = nullptr;

		struct llama_model*       Model       = nullptr;
		struct llama_context*     Context     = nullptr;
		const struct llama_vocab* Vocab       = nullptr;
		llama_token               StopTokenId = -1;

		TUniquePtr<FInoOnnxSession> Decoder;

		FString ModelDesc;

		/**
		 * Optional cache of the post-prefix KV state for the active voice.
		 * TUniquePtr (not inline) so HasCachedVoice + ClearVoiceCache are
		 * O(1) and the multi-MB snapshot byte array stays heap-allocated.
		 */
		TUniquePtr<FInoNeuTtsVoiceCache> VoiceCache;
	};
}
