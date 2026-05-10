// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsRunner.h"
#include "InoNeuTtsCommon.h"        // TokenizePrompt
#include "InoNeuTtsLog.h"
#include "InoNeuTtsPromptBuilder.h" // BuildSpeechCodeTokensString (needed indirectly)

#include "InoSpeakNGBPLibrary.h"    // Phonemize for the cacheable RefPhones

#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

#include "Onnx/InoOnnxSession.h"
#include "Onnx/InoOnnxTensor.h"
#include "Onnx/InoOnnxTypes.h"

namespace InoNeuTtsNative
{
	namespace
	{
		/** Human-readable name for an EInoLlamaKvDtype (logs only). */
		const TCHAR* KvDtypeName(EInoLlamaKvDtype D)
		{
			switch (D)
			{
				case EInoLlamaKvDtype::F16:  return TEXT("F16");
				case EInoLlamaKvDtype::F32:  return TEXT("F32");
				case EInoLlamaKvDtype::Q8_0: return TEXT("Q8_0");
				case EInoLlamaKvDtype::Q4_0: return TEXT("Q4_0");
			}
			return TEXT("?");
		}

		/** Human-readable name for an EInoLlamaFlashAttnType (logs only). */
		const TCHAR* FlashAttnName(EInoLlamaFlashAttnType F)
		{
			switch (F)
			{
				case EInoLlamaFlashAttnType::Auto:     return TEXT("Auto");
				case EInoLlamaFlashAttnType::Disabled: return TEXT("Off");
				case EInoLlamaFlashAttnType::Enabled:  return TEXT("On");
			}
			return TEXT("?");
		}

		/** Human-readable name for an EInoOnnxProvider (logs only). */
		const TCHAR* OnnxProviderName(EInoOnnxProvider P)
		{
			switch (P)
			{
				case EInoOnnxProvider::Cpu:       return TEXT("CPU");
				case EInoOnnxProvider::Xnnpack:   return TEXT("XNNPACK");
				case EInoOnnxProvider::Nnapi:     return TEXT("NNAPI");
				case EInoOnnxProvider::WebGpu:    return TEXT("WebGPU");
				case EInoOnnxProvider::DirectMl:  return TEXT("DirectML");
				case EInoOnnxProvider::Cuda:      return TEXT("CUDA");
				case EInoOnnxProvider::TensorRt:  return TEXT("TensorRT");
			}
			return TEXT("?");
		}

		/** Comma-joined list of registered ggml backends — "CPU, Vulkan", etc. */
		FString RegisteredGgmlBackends(const InoAgents::LlamaCpp::FLlamaCppApi& Api)
		{
			if (Api.ggml_backend_reg_count == nullptr ||
				Api.ggml_backend_reg_get   == nullptr ||
				Api.ggml_backend_reg_name  == nullptr)
			{
				return TEXT("?");
			}
			const size_t N = Api.ggml_backend_reg_count();
			TArray<FString> Names;
			Names.Reserve((int32)N);
			for (size_t i = 0; i < N; ++i)
			{
				if (struct ggml_backend_reg* Reg = Api.ggml_backend_reg_get(i))
				{
					if (const char* Name = Api.ggml_backend_reg_name(Reg))
					{
						Names.Add(FString(UTF8_TO_TCHAR(Name)));
					}
				}
			}
			return FString::Join(Names, TEXT(", "));
		}

		/**
		 * Look up the token id of a literal special-token string by
		 * tokenizing it with parse_special=true. NeuTTS's prompt template
		 * uses several special tokens (e.g. <|SPEECH_GENERATION_END|>,
		 * <|TEXT_PROMPT_START|>, <|speech_N|>) that the tokenizer
		 * resolves to single ids when parse_special is set.
		 *
		 * Returns -1 (and logs) on any failure — most likely cause is a
		 * vocab mismatch (the GGUF wasn't a NeuTTS-flavoured Qwen2 model).
		 */
		llama_token ResolveSingleSpecialToken(
			const InoAgents::LlamaCpp::FLlamaCppApi& Api,
			const struct llama_vocab* Vocab,
			const ANSICHAR* Text)
		{
			llama_token Buf[8];
			const int32 ByteLen = static_cast<int32>(FCStringAnsi::Strlen(Text));

			const int32 N = Api.llama_tokenize(
				Vocab,
				Text,
				ByteLen,
				Buf,
				UE_ARRAY_COUNT(Buf),
				/*add_special*/ false,
				/*parse_special*/ true);

			if (N != 1)
			{
				UE_LOG(LogInoNeuTts, Error,
					TEXT("Special-token resolve for '%s' returned %d tokens (expected 1)."),
					ANSI_TO_TCHAR(Text), N);
				return -1;
			}
			return Buf[0];
		}
	}

	FInoNeuTtsRunner::~FInoNeuTtsRunner()
	{
		// Decoder doesn't depend on llama state; free it first so its
		// dtor logs land before the (much louder) llama dtor logs.
		Decoder.Reset();

		if (LlamaApi != nullptr)
		{
			if (Context != nullptr)
			{
				LlamaApi->llama_free(Context);
				Context = nullptr;
			}
			if (Model != nullptr)
			{
				LlamaApi->llama_model_free(Model);
				Model = nullptr;
			}
		}
	}

	TUniquePtr<FInoNeuTtsRunner> FInoNeuTtsRunner::Create(
		const FInoNeuTtsConfig& Config,
		const FString& GgufPath,
		const FString& OnnxDecoderPath,
		FString& OutError)
	{
		const auto* Api = InoAgents::LlamaCpp::GetApi();
		if (Api == nullptr)
		{
			OutError = TEXT("InoLlama vtable unavailable. ")
				TEXT("llama.cpp runtime may not be staged for this platform.");
			UE_LOG(LogInoNeuTts, Error, TEXT("%s"), *OutError);
			return nullptr;
		}

		if (!FPaths::FileExists(GgufPath))
		{
			OutError = FString::Printf(
				TEXT("GGUF model not found at '%s'. ")
				TEXT("Confirm the file exists under Plugins/InoAgents/NeuTTS/models/."),
				*GgufPath);
			UE_LOG(LogInoNeuTts, Error, TEXT("%s"), *OutError);
			return nullptr;
		}

		if (!FPaths::FileExists(OnnxDecoderPath))
		{
			OutError = FString::Printf(
				TEXT("NeuCodec ONNX decoder not found at '%s'."),
				*OnnxDecoderPath);
			UE_LOG(LogInoNeuTts, Error, TEXT("%s"), *OutError);
			return nullptr;
		}

		// Allocate runner first — we want the destructor to clean up any
		// partial state if a later step fails.
		TUniquePtr<FInoNeuTtsRunner> Runner(new FInoNeuTtsRunner());
		Runner->LlamaApi = Api;

		// ---- Load GGUF backbone --------------------------------------
		// Delegate the boilerplate (default params + ApplyTo +
		// llama_model_load_from_file + error handling + timing log) to
		// InoLlama's generic helper. The Runner just supplies the path
		// and forwards Config.Backbone.
		Runner->Model = InoAgents::LlamaCpp::LoadModelFromFile(
			GgufPath, Config.Backbone, &OutError);
		if (Runner->Model == nullptr)
		{
			// Helper already logged + populated OutError.
			return nullptr;
		}

		Runner->Vocab = Api->llama_model_get_vocab(Runner->Model);
		if (Runner->Vocab == nullptr)
		{
			OutError = TEXT("llama_model_get_vocab returned null.");
			UE_LOG(LogInoNeuTts, Error, TEXT("%s"), *OutError);
			return nullptr;
		}

		// Pull a human-readable description for diagnostics. Never fatal.
		{
			char DescBuf[256] = {};
			const int32 DescLen = Api->llama_model_desc(Runner->Model, DescBuf, sizeof(DescBuf));
			if (DescLen > 0)
			{
				Runner->ModelDesc = FString(UTF8_TO_TCHAR(DescBuf));
			}
		}

		// ---- Create inference context --------------------------------
		Runner->Context = InoAgents::LlamaCpp::CreateContext(
			Runner->Model, Config.BackboneContext, &OutError);
		if (Runner->Context == nullptr)
		{
			return nullptr;
		}

		// ---- Resolve <|SPEECH_GENERATION_END|> stop token id ---------
		Runner->StopTokenId = ResolveSingleSpecialToken(
			*Api, Runner->Vocab, "<|SPEECH_GENERATION_END|>");
		if (Runner->StopTokenId < 0)
		{
			OutError = TEXT("Failed to resolve <|SPEECH_GENERATION_END|> token id ")
			           TEXT("(GGUF may not be a NeuTTS Nano/Air model).");
			UE_LOG(LogInoNeuTts, Error, TEXT("%s"), *OutError);
			return nullptr;
		}

		// ---- Load NeuCodec ONNX decoder ------------------------------
		// Use the user-supplied DecoderOnnx config verbatim. Default-
		// constructed it's CPU-only with full graph optimization, which
		// matches Neuphonic's reference (their NeuCodecOnnxDecoder
		// enforces CPU). Callers can opt into DirectML / NNAPI / etc.
		// by populating Config.DecoderOnnx.ExecutionProviders.
		FString OnnxError;
		Runner->Decoder = FInoOnnxSession::Create(
			OnnxDecoderPath, Config.DecoderOnnx, &OnnxError);
		if (!Runner->Decoder.IsValid())
		{
			OutError = FString::Printf(
				TEXT("Failed to load NeuCodec ONNX decoder at '%s': %s"),
				*OnnxDecoderPath, *OnnxError);
			UE_LOG(LogInoNeuTts, Error, TEXT("%s"), *OutError);
			return nullptr;
		}

		// ---- Decoder warmup -------------------------------------------
		// The dummy input is small enough that the cost is bounded
		// (~50–200 ms) and the model just decodes zeros into silence,
		// which Warmup discards. NeuCodec accepts any int32 codes in
		// [0, 65535], so all-zero input is well-formed. Skipped if
		// Config.bWarmupDecoderOnLoad is false.
		if (Config.bWarmupDecoderOnLoad)
		{
			const TArray<int32> DummyCodes = { 0, 0, 0, 0, 0, 0, 0, 0 };
			const TArray<int64> DummyShape = { 1, 1, DummyCodes.Num() };
			FInoOnnxTensor DummyInput = FInoOnnxTensor::CreateFromBufferCopy<int32>(
				DummyShape, TArrayView<const int32>(DummyCodes));

			if (DummyInput.IsValid())
			{
				const double DecoderWarmT0 = FPlatformTime::Seconds();
				TArray<FInoOnnxTensor> DummyInputs;
				DummyInputs.Add(MoveTemp(DummyInput));
				Runner->Decoder->Warmup(DummyInputs);
				const double DecoderWarmMs =
					(FPlatformTime::Seconds() - DecoderWarmT0) * 1000.0;
				UE_LOG(LogInoNeuTts, Log,
					TEXT("Decoder warmup: %.2f ms (%d dummy codes -> ONNX Run)"),
					DecoderWarmMs, DummyCodes.Num());
			}
		}

		// ---- Backbone warmup -------------------------------------------
		// Run a tiny prefill + 1-token decode through llama_decode to pay
		// the cold-start costs (kernel JIT on Vulkan, KV cache allocation,
		// per-layer init) once at load time. The dummy uses BOS-only input
		// (always present in the vocab); the resulting logits are discarded.
		// Cleared from KV right after via llama_memory_clear so the actual
		// synth path starts from a clean slate. Skipped if
		// Config.bWarmupBackboneOnLoad is false or the BOS token isn't
		// resolvable for any reason.
		if (Config.bWarmupBackboneOnLoad)
		{
			const double WarmupT0 = FPlatformTime::Seconds();
			llama_token BosToken = -1;
			if (Api->llama_vocab_get_add_bos != nullptr)
			{
				// BOS isn't always exposed by the vocab — fall back to
				// resolving "<|TEXT_PROMPT_START|>" which we know exists
				// in the NeuTTS-extended Qwen2 vocab.
				BosToken = ResolveSingleSpecialToken(
					*Api, Runner->Vocab, "<|TEXT_PROMPT_START|>");
			}

			if (BosToken >= 0)
			{
				llama_memory_t Mem = Api->llama_get_memory(Runner->Context);
				Api->llama_memory_clear(Mem, /*data*/ true);

				llama_token DummyTokens[1] = { BosToken };
				struct llama_batch DummyBatch =
					Api->llama_batch_get_one(DummyTokens, 1);
				if (Api->llama_decode(Runner->Context, DummyBatch) == 0)
				{
					const double WarmupSec = FPlatformTime::Seconds() - WarmupT0;
					const double TokPerSec = WarmupSec > 0.0 ? 1.0 / WarmupSec : 0.0;
					UE_LOG(LogInoNeuTts, Log,
						TEXT("Backbone warmup: 1 tok in %.2f ms (%.0f tok/s @ first decode — ")
						TEXT("includes kernel JIT + KV alloc on Vulkan)"),
						WarmupSec * 1000.0, TokPerSec);
				}
				// Restore an empty KV state so the first real synth (or
				// PrimeVoice) starts clean.
				Api->llama_memory_clear(Mem, /*data*/ true);
			}
		}

		// ---- Runner-ready summary (single multi-line block) -----------
		// Pulls the EFFECTIVE values from the loaded model + context so
		// the log reflects what's actually running, not what was requested.
		// Anything not exposed by the vtable (e.g. older llama.dll without
		// the diagnostic getters) shows as "?".
		const uint32 EffNCtx       = Api->llama_n_ctx(Runner->Context);
		const int32  EffNThreads   = (Api->llama_n_threads != nullptr)
			? Api->llama_n_threads(Runner->Context) : -1;
		const int32  EffNThreadsBp = (Api->llama_n_threads_batch != nullptr)
			? Api->llama_n_threads_batch(Runner->Context) : -1;
		const uint64 ModelParams   = (Api->llama_model_n_params != nullptr)
			? Api->llama_model_n_params(Runner->Model) : 0;
		const uint64 ModelBytes    = (Api->llama_model_size != nullptr)
			? Api->llama_model_size(Runner->Model) : 0;
		const int32  ModelLayers   = (Api->llama_model_n_layer != nullptr)
			? Api->llama_model_n_layer(Runner->Model) : -1;
		const bool   bGpuBuild     = (Api->llama_supports_gpu_offload != nullptr)
			? Api->llama_supports_gpu_offload() : false;
		const FString GgmlBackends = RegisteredGgmlBackends(*Api);

		// "Effective" GPU offload — clamp to layer count when known.
		const int32 ReqGpuLayers   = Config.Backbone.NumGpuLayers;
		const int32 EffGpuLayers   = (ModelLayers > 0)
			? FMath::Min(ReqGpuLayers, ModelLayers)
			: ReqGpuLayers;

		// Decoder providers list — flatten the EInoOnnxProvider array to
		// a "DirectMl, Cpu" string.
		FString DecoderProviders;
		if (Runner->Decoder.IsValid())
		{
			const TArray<EInoOnnxProvider>& Active =
				Runner->Decoder->GetActiveProviders();
			TArray<FString> Names;
			Names.Reserve(Active.Num());
			for (EInoOnnxProvider P : Active)
			{
				Names.Add(OnnxProviderName(P));
			}
			DecoderProviders = (Names.Num() > 0)
				? FString::Join(Names, TEXT(", "))
				: TEXT("(none)");
		}

		UE_LOG(LogInoNeuTts, Log,
			TEXT("====================================================="));
		UE_LOG(LogInoNeuTts, Log,
			TEXT("NeuTTS Runner ready"));
		UE_LOG(LogInoNeuTts, Log,
			TEXT("  Backbone:    '%s'"), *Runner->ModelDesc);
		UE_LOG(LogInoNeuTts, Log,
			TEXT("    params=%.0fM  size=%.1f MB  layers=%d  stop_token=%d"),
			ModelParams / 1.0e6,
			ModelBytes / (1024.0 * 1024.0),
			ModelLayers,
			Runner->StopTokenId);
		UE_LOG(LogInoNeuTts, Log,
			TEXT("    n_ctx=%u  threads=%d (batch=%d)  flash_attn=%s  ")
			TEXT("kv_k=%s  kv_v=%s  offload_kqv=%s"),
			EffNCtx,
			EffNThreads,
			EffNThreadsBp,
			FlashAttnName(Config.BackboneContext.FlashAttnType),
			KvDtypeName(Config.BackboneContext.KvDtypeK),
			KvDtypeName(Config.BackboneContext.KvDtypeV),
			Config.BackboneContext.bOffloadKQV ? TEXT("yes") : TEXT("no"));
		UE_LOG(LogInoNeuTts, Log,
			TEXT("    gpu_layers=%d (requested=%d, build_supports_gpu=%s)"),
			EffGpuLayers, ReqGpuLayers,
			bGpuBuild ? TEXT("yes") : TEXT("no"));
		UE_LOG(LogInoNeuTts, Log,
			TEXT("    ggml_backends_registered: [%s]"), *GgmlBackends);
		if (ReqGpuLayers > 0 && !bGpuBuild)
		{
			UE_LOG(LogInoNeuTts, Warning,
				TEXT("  -> NumGpuLayers=%d requested but this llama build has NO GPU backend; ")
				TEXT("inference will run on CPU."),
				ReqGpuLayers);
		}
		UE_LOG(LogInoNeuTts, Log,
			TEXT("  Decoder:     I/O=%d->%d  active_providers=[%s]"),
			Runner->Decoder->GetInputCount(),
			Runner->Decoder->GetOutputCount(),
			*DecoderProviders);
		UE_LOG(LogInoNeuTts, Log,
			TEXT("====================================================="));

		return Runner;
	}

	// ====================================================================
	//  Voice cache (KV snapshot of the prompt prefix)
	// ====================================================================

	void FInoNeuTtsRunner::ClearVoiceCache()
	{
		VoiceCache.Reset();
	}

	bool FInoNeuTtsRunner::HasCachedVoice(const FString& VoiceName) const
	{
		return VoiceCache.IsValid()
			&& VoiceCache->IsValid()
			&& VoiceCache->VoiceName.Equals(VoiceName, ESearchCase::IgnoreCase);
	}

	bool FInoNeuTtsRunner::PrimeVoice(const FInoNeuTtsVoice& Voice, FString& OutError)
	{
		const double T0 = FPlatformTime::Seconds();

		if (LlamaApi == nullptr || Context == nullptr || Vocab == nullptr)
		{
			OutError = TEXT("PrimeVoice: runner not initialized.");
			return false;
		}
		if (!Voice.bIsValid || Voice.RefCodes.Num() == 0)
		{
			OutError = TEXT("PrimeVoice: voice has no ref_codes (load via voice registry first).");
			return false;
		}

		const auto& Api = *LlamaApi;

		// Hard requirement: the snapshot APIs were added to the InoLlama
		// vtable as part of voice caching. If they're null, the host's
		// llama.dll / libllama.so doesn't expose them — fall back at the
		// caller (caller treats prime failure as "no cache, slow path").
		if (Api.llama_state_seq_get_size == nullptr ||
			Api.llama_state_seq_get_data == nullptr ||
			Api.llama_state_seq_set_data == nullptr ||
			Api.llama_get_memory         == nullptr ||
			Api.llama_memory_clear       == nullptr)
		{
			OutError = TEXT("PrimeVoice: state_seq snapshot API not available in InoLlama vtable.");
			return false;
		}

		// ---- 1. Resolve RefPhones (lazy phonemize if not pre-baked) ----
		FString ResolvedRefPhones = Voice.RefPhones;
		if (ResolvedRefPhones.IsEmpty())
		{
			ResolvedRefPhones =
				UInoSpeakNGBPLibrary::Phonemize(Voice.RefText, Voice.Language);
			if (ResolvedRefPhones.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("PrimeVoice: phonemization of voice ref_text failed for language '%s'."),
					*Voice.Language);
				return false;
			}
		}

		// Normalize whitespace to match vendor's `phones.split() + " ".join()`.
		// eSpeak occasionally emits per-clause leading/trailing whitespace, and
		// our clause-joining produces double spaces between clauses — both
		// shift tokenization in subtle ways. Idempotent on already-clean input.
		ResolvedRefPhones = NormalizePhones(ResolvedRefPhones);

		// ---- 2. Build + tokenize the cacheable prefix ----
		const FString PrefixString = BuildSynthesisPromptPrefix(ResolvedRefPhones);

		TArray<llama_token> PrefixTokens;
		FString TokenizeError;
		if (!TokenizePrompt(Api, Vocab, PrefixString, PrefixTokens, TokenizeError))
		{
			OutError = FString::Printf(
				TEXT("PrimeVoice: prefix tokenize failed: %s"), *TokenizeError);
			return false;
		}

		const uint32 NCtx = Api.llama_n_ctx(Context);
		if ((uint32)PrefixTokens.Num() >= NCtx)
		{
			OutError = FString::Printf(
				TEXT("PrimeVoice: prefix size %d exceeds context %u."),
				PrefixTokens.Num(), NCtx);
			return false;
		}

		// ---- 3. Clear KV + prefill the prefix into seq 0 ----
		llama_memory_t Mem = Api.llama_get_memory(Context);
		Api.llama_memory_clear(Mem, /*data*/ true);

		struct llama_batch PrefillBatch =
			Api.llama_batch_get_one(PrefixTokens.GetData(), PrefixTokens.Num());
		if (Api.llama_decode(Context, PrefillBatch) != 0)
		{
			OutError = TEXT("PrimeVoice: llama_decode (prefill) failed.");
			Api.llama_memory_clear(Mem, /*data*/ true);
			return false;
		}

		// ---- 4. Snapshot KV state for sequence 0 ----
		const size_t StateSize = Api.llama_state_seq_get_size(Context, /*seq_id*/ 0);
		if (StateSize == 0)
		{
			OutError = TEXT("PrimeVoice: llama_state_seq_get_size returned 0.");
			Api.llama_memory_clear(Mem, /*data*/ true);
			return false;
		}

		TArray<uint8> Snapshot;
		Snapshot.SetNumUninitialized(static_cast<int32>(StateSize));

		const size_t Written = Api.llama_state_seq_get_data(
			Context, Snapshot.GetData(), StateSize, /*seq_id*/ 0);
		if (Written == 0 || Written > StateSize)
		{
			OutError = FString::Printf(
				TEXT("PrimeVoice: llama_state_seq_get_data wrote %llu bytes (expected up to %llu)."),
				static_cast<unsigned long long>(Written),
				static_cast<unsigned long long>(StateSize));
			Api.llama_memory_clear(Mem, /*data*/ true);
			return false;
		}
		// Trim to the actually-written size — _get_size returns an upper
		// bound; the written count is the canonical state size.
		Snapshot.SetNum(static_cast<int32>(Written), EAllowShrinking::No);

		// ---- 5. Pre-build the speech-tokens block string ----
		// Done once at prime time. Saves 650 FString::Printf + concat calls
		// per synth and a meaningful amount of allocation pressure.
		FString SpeechTokensBlock = BuildSpeechTokensBlock(Voice.RefCodes);

		// ---- 6. Commit to cache ----
		TUniquePtr<FInoNeuTtsVoiceCache> NewCache(new FInoNeuTtsVoiceCache());
		NewCache->VoiceName         = Voice.Name;
		NewCache->ResolvedRefPhones = MoveTemp(ResolvedRefPhones);
		NewCache->SpeechTokensBlock = MoveTemp(SpeechTokensBlock);
		NewCache->PrefixTokens      = MoveTemp(PrefixTokens);
		NewCache->KvSnapshot        = MoveTemp(Snapshot);

		VoiceCache = MoveTemp(NewCache);

		const double Elapsed = FPlatformTime::Seconds() - T0;
		UE_LOG(LogInoNeuTts, Log,
			TEXT("Voice cache primed: '%s' prefix=%d tokens, snapshot=%d bytes, ")
			TEXT("speech-block=%d chars, %.2f ms"),
			*VoiceCache->VoiceName,
			VoiceCache->PrefixTokens.Num(),
			VoiceCache->KvSnapshot.Num(),
			VoiceCache->SpeechTokensBlock.Len(),
			Elapsed * 1000.0);

		return true;
	}
}
