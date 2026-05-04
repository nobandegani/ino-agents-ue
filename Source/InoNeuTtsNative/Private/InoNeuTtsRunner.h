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
		EInoNeuTtsVariant                        GetVariant()  const { return Variant;  }
		const FString&                           GetModelDescription() const { return ModelDesc; }

	private:
		FInoNeuTtsRunner() = default;

		const InoAgents::LlamaCpp::FLlamaCppApi* LlamaApi = nullptr;

		struct llama_model*       Model       = nullptr;
		struct llama_context*     Context     = nullptr;
		const struct llama_vocab* Vocab       = nullptr;
		llama_token               StopTokenId = -1;

		TUniquePtr<FInoOnnxSession> Decoder;

		EInoNeuTtsVariant Variant = EInoNeuTtsVariant::Nano;
		FString           ModelDesc;
	};
}
