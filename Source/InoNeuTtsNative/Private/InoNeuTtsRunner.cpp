// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsRunner.h"
#include "InoNeuTtsLog.h"

#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

#include "Onnx/InoOnnxSession.h"
#include "Onnx/InoOnnxTensor.h"
#include "Onnx/InoOnnxTypes.h"

namespace InoNeuTtsNative
{
	namespace
	{
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
		Runner->Variant  = Config.Variant;

		// ---- Load GGUF backbone --------------------------------------
		struct llama_model_params ModelParams = Api->llama_model_default_params();
		ModelParams.n_gpu_layers = Config.NumGpuLayers;

		const FTCHARToUTF8 GgufPathUtf8(*GgufPath);
		Runner->Model = Api->llama_model_load_from_file(GgufPathUtf8.Get(), ModelParams);
		if (Runner->Model == nullptr)
		{
			OutError = FString::Printf(
				TEXT("llama_model_load_from_file failed for '%s'."), *GgufPath);
			UE_LOG(LogInoNeuTts, Error, TEXT("%s"), *OutError);
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
		struct llama_context_params CtxParams = Api->llama_context_default_params();
		CtxParams.n_ctx = static_cast<uint32_t>(Config.ContextSize);
		if (Config.NumThreads > 0)
		{
			CtxParams.n_threads       = Config.NumThreads;
			CtxParams.n_threads_batch = Config.NumThreads;
		}

		Runner->Context = Api->llama_init_from_model(Runner->Model, CtxParams);
		if (Runner->Context == nullptr)
		{
			OutError = TEXT("llama_init_from_model returned null.");
			UE_LOG(LogInoNeuTts, Error, TEXT("%s"), *OutError);
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
				TArray<FInoOnnxTensor> DummyInputs;
				DummyInputs.Add(MoveTemp(DummyInput));
				Runner->Decoder->Warmup(DummyInputs);
			}
		}

		UE_LOG(LogInoNeuTts, Log,
			TEXT("Runner ready. variant=%s n_ctx=%u stop=%d decoder I/O=%d->%d desc='%s'"),
			Runner->Variant == EInoNeuTtsVariant::Nano ? TEXT("Nano") : TEXT("Air"),
			Api->llama_n_ctx(Runner->Context),
			Runner->StopTokenId,
			Runner->Decoder->GetInputCount(),
			Runner->Decoder->GetOutputCount(),
			*Runner->ModelDesc);

		return Runner;
	}
}
