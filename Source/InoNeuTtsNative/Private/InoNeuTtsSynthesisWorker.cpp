// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsSynthesisWorker.h"
#include "InoNeuTtsLog.h"
#include "InoNeuTtsPromptBuilder.h"
#include "InoNeuTtsRunner.h"

#include "Audio/InoAudioFunctionLibrary.h"
#include "InoSpeakNGBPLibrary.h"

#include "Onnx/InoOnnxSession.h"
#include "Onnx/InoOnnxTensor.h"
#include "Onnx/InoOnnxTypes.h"

#include "HAL/PlatformTime.h"
#include "Internationalization/Regex.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/ScopeExit.h"

namespace InoNeuTtsNative
{
	namespace
	{
		/** AR-loop cancel-flag check cadence (token count). Cheap atomic load. */
		constexpr int32 kCancelCheckEvery = 64;

		/** Codec hop length: 50 Hz code rate at 24 kHz output. */
		constexpr int32 kCodecHopLength = 480;

		/** Output sample rate, baked into NeuCodec. */
		constexpr int32 kSampleRate = 24000;

		/**
		 * Helper to make an FInoNeuTtsResult representing a hard failure.
		 * Logs at Error level and returns a populated result.
		 */
		FInoNeuTtsResult MakeFailure(const FString& Message)
		{
			UE_LOG(LogInoNeuTts, Error, TEXT("Synthesis failed: %s"), *Message);
			FInoNeuTtsResult R;
			R.bSuccess = false;
			R.ErrorMessage = Message;
			R.SampleRate = kSampleRate;
			return R;
		}

		/**
		 * Tokenize a UTF-8 string with parse_special=true. Two-pass: first
		 * call probes required size by passing a too-small buffer (returns
		 * negative count = -required), second call writes for real.
		 *
		 * add_special is ALWAYS false for NeuTTS — the chat template
		 * already contains every special-token string in plain text, so
		 * letting the tokenizer auto-prepend a BOS / system would corrupt
		 * the prompt structure.
		 */
		bool TokenizePrompt(
			const InoAgents::LlamaCpp::FLlamaCppApi& Api,
			const struct llama_vocab* Vocab,
			const FString& Prompt,
			TArray<llama_token>& OutTokens,
			FString& OutError)
		{
			const FTCHARToUTF8 PromptUtf8(*Prompt);
			const int32 ByteLen = PromptUtf8.Length();

			// Probe: a stack buffer big enough for most prompts. If too
			// small, llama_tokenize returns -<required>.
			llama_token Probe[8];
			const int32 N = Api.llama_tokenize(
				Vocab,
				PromptUtf8.Get(), ByteLen,
				Probe, UE_ARRAY_COUNT(Probe),
				/*add_special*/ false,
				/*parse_special*/ true);

			int32 Required = N;
			if (N < 0)
			{
				Required = -N;
			}
			else if (N > 0)
			{
				// Fits in the probe; copy out.
				OutTokens.SetNumUninitialized(N);
				FMemory::Memcpy(OutTokens.GetData(), Probe, sizeof(llama_token) * N);
				return true;
			}
			else
			{
				OutError = TEXT("llama_tokenize produced 0 tokens.");
				return false;
			}

			// Allocate the real buffer and re-tokenize.
			OutTokens.SetNumUninitialized(Required);
			const int32 N2 = Api.llama_tokenize(
				Vocab,
				PromptUtf8.Get(), ByteLen,
				OutTokens.GetData(), OutTokens.Num(),
				/*add_special*/ false,
				/*parse_special*/ true);

			if (N2 != Required)
			{
				OutError = FString::Printf(
					TEXT("llama_tokenize second call returned %d (expected %d)."),
					N2, Required);
				return false;
			}
			return true;
		}

		/**
		 * Convert one llama_token id to its piece text and append to
		 * OutText. Tries a stack buffer first; falls back to heap on the
		 * rare oversize case (long custom special tokens).
		 */
		void AppendTokenPiece(
			const InoAgents::LlamaCpp::FLlamaCppApi& Api,
			const struct llama_vocab* Vocab,
			llama_token Token,
			FString& OutText)
		{
			char Stack[256];
			int32 Len = Api.llama_token_to_piece(
				Vocab, Token, Stack, UE_ARRAY_COUNT(Stack),
				/*lstrip*/ 0, /*special*/ true);

			if (Len > 0)
			{
				Stack[Len] = '\0';
				OutText += UTF8_TO_TCHAR(Stack);
				return;
			}

			if (Len < 0)
			{
				// Need a bigger buffer.
				const int32 Required = -Len + 1;
				TArray<char> Heap;
				Heap.SetNumUninitialized(Required);
				const int32 Len2 = Api.llama_token_to_piece(
					Vocab, Token, Heap.GetData(), Heap.Num(),
					0, true);
				if (Len2 > 0)
				{
					Heap[Len2] = '\0';
					OutText += UTF8_TO_TCHAR(Heap.GetData());
				}
			}
		}

		/**
		 * Regex-extract every <|speech_NNN|> id from the generated text
		 * into an int32 array.
		 */
		TArray<int32> ParseSpeechTokenIds(const FString& Generated)
		{
			TArray<int32> Ids;

			static const FRegexPattern Pattern(TEXT("<\\|speech_(\\d+)\\|>"));
			FRegexMatcher Matcher(Pattern, Generated);
			while (Matcher.FindNext())
			{
				const FString IdStr = Matcher.GetCaptureGroup(1);
				Ids.Add(FCString::Atoi(*IdStr));
			}
			return Ids;
		}

		/**
		 * Run the AR generation loop after prefill has populated the KV
		 * cache. Stop on <|SPEECH_GENERATION_END|>, generic EOG, MaxNewTokens,
		 * or cancel.
		 *
		 * Returns the count of tokens appended, with each token's piece
		 * text appended to OutGeneratedText.
		 */
		int32 RunArLoop(
			const InoAgents::LlamaCpp::FLlamaCppApi& Api,
			struct llama_context* Ctx,
			const struct llama_vocab* Vocab,
			llama_token StopTokenId,
			struct llama_sampler* Chain,
			int32 MaxNewTokens,
			const std::atomic<bool>* CancelFlag,
			FString& OutGeneratedText,
			bool& bOutCancelled)
		{
			bOutCancelled = false;
			int32 Generated = 0;

			for (int32 Iter = 0; Iter < MaxNewTokens; ++Iter)
			{
				if (CancelFlag != nullptr &&
					(Iter % kCancelCheckEvery) == 0 &&
					CancelFlag->load(std::memory_order_relaxed))
				{
					bOutCancelled = true;
					break;
				}

				// idx=-1 -> sample from logits at the last position
				llama_token Next = Api.llama_sampler_sample(Chain, Ctx, -1);

				// Stop conditions
				if (Next == StopTokenId)
				{
					break;
				}
				if (Api.llama_vocab_is_eog(Vocab, Next))
				{
					break;
				}

				// Capture the token's text and notify the sampler chain.
				AppendTokenPiece(Api, Vocab, Next, OutGeneratedText);
				Api.llama_sampler_accept(Chain, Next);
				Generated++;

				// Feed the new token back to advance the KV cache.
				struct llama_batch StepBatch =
					Api.llama_batch_get_one(&Next, 1);
				if (Api.llama_decode(Ctx, StepBatch) != 0)
				{
					UE_LOG(LogInoNeuTts, Error,
						TEXT("llama_decode failed at AR step %d."), Iter);
					break;
				}
			}

			return Generated;
		}

		/**
		 * Build the full sampler chain matching Neuphonic's reference
		 * Python defaults: top_k(50) -> temp(1.0) -> dist(seed).
		 *
		 * The chain takes ownership of each added sub-sampler. Chain
		 * itself is owned by the caller (free with llama_sampler_free).
		 */
		struct llama_sampler* BuildSamplerChain(
			const InoAgents::LlamaCpp::FLlamaCppApi& Api,
			const FInoNeuTtsOptions& Options)
		{
			struct llama_sampler_chain_params Params =
				Api.llama_sampler_chain_default_params();
			struct llama_sampler* Chain = Api.llama_sampler_chain_init(Params);

			Api.llama_sampler_chain_add(Chain, Api.llama_sampler_init_top_k(Options.TopK));
			Api.llama_sampler_chain_add(Chain, Api.llama_sampler_init_temp(Options.Temperature));

			const uint32 Seed = (Options.RandomSeed < 0)
				? static_cast<uint32>(FPlatformTime::Cycles())
				: static_cast<uint32>(Options.RandomSeed);
			Api.llama_sampler_chain_add(Chain, Api.llama_sampler_init_dist(Seed));

			return Chain;
		}

		/**
		 * Decode an int32 array of speech-token ids through the NeuCodec
		 * ONNX session. Output shape is [1, 1, N * 480] float32 in [-1, +1].
		 *
		 * Writes the float32 audio into OutFloat. Returns false on any
		 * error (logs at Error level).
		 */
		bool DecodeSpeechTokens(
			FInoOnnxSession& Decoder,
			const TArray<int32>& SpeechIds,
			TArray<float>& OutFloat,
			FString& OutError)
		{
			// Input shape [1, 1, N] int32 — Neuphonic's onnx_example.py
			// passes codes shaped like this and the model uses a single
			// input tensor.
			const int64 N = SpeechIds.Num();
			const TArray<int64> Shape = { 1, 1, N };

			FInoOnnxTensor InTensor =
				FInoOnnxTensor::CreateFromBufferCopy<int32>(
					Shape,
					TArrayView<const int32>(SpeechIds));

			if (!InTensor.IsValid())
			{
				OutError = TEXT("Failed to build int32 input tensor for NeuCodec decoder.");
				return false;
			}

			TArray<FInoOnnxTensor> Inputs;
			Inputs.Add(MoveTemp(InTensor));

			TArray<FInoOnnxTensor> Outputs;
			if (!Decoder.Run(Inputs, Outputs, &OutError))
			{
				return false;
			}

			if (Outputs.Num() == 0 || !Outputs[0].IsValid())
			{
				OutError = TEXT("NeuCodec decoder returned no output tensors.");
				return false;
			}

			Outputs[0].CopyToArray<float>(OutFloat);
			if (OutFloat.Num() == 0)
			{
				OutError = TEXT("NeuCodec decoder output tensor was empty.");
				return false;
			}

			return true;
		}
	}

	FInoNeuTtsResult RunSynthesis(
		FInoNeuTtsRunner& Runner,
		const FString& InputText,
		FInoNeuTtsVoice& Voice,
		const FInoNeuTtsOptions& Options,
		const std::atomic<bool>* CancelFlag)
	{
		const double T0 = FPlatformTime::Seconds();

		if (InputText.IsEmpty())
		{
			return MakeFailure(TEXT("InputText is empty."));
		}
		if (!Voice.bIsValid || Voice.RefCodes.Num() == 0)
		{
			return MakeFailure(TEXT("Voice has no ref_codes (load via voice registry first)."));
		}

		const auto& Api = Runner.GetLlamaApi();
		const struct llama_vocab* Vocab = Runner.GetVocab();
		struct llama_context* Ctx = Runner.GetContext();

		// ---- 1. Phonemize input text + lazy-phonemize voice ref text ----
		const FString InputPhones =
			UInoSpeakNGBPLibrary::Phonemize(InputText, Voice.Language);
		if (InputPhones.IsEmpty())
		{
			return MakeFailure(FString::Printf(
				TEXT("Phonemization of input failed for language '%s'. ")
				TEXT("Is InoSpeakNG initialized?"),
				*Voice.Language));
		}

		if (Voice.RefPhones.IsEmpty())
		{
			Voice.RefPhones =
				UInoSpeakNGBPLibrary::Phonemize(Voice.RefText, Voice.Language);
			if (Voice.RefPhones.IsEmpty())
			{
				return MakeFailure(FString::Printf(
					TEXT("Phonemization of voice ref_text failed for language '%s'."),
					*Voice.Language));
			}
		}

		// ---- 2. Build prompt ----
		const FString Prompt = BuildSynthesisPrompt(
			Voice.RefPhones, InputPhones, Voice.RefCodes);

		// ---- 3. Tokenize ----
		TArray<llama_token> PromptTokens;
		FString TokenizeError;
		if (!TokenizePrompt(Api, Vocab, Prompt, PromptTokens, TokenizeError))
		{
			return MakeFailure(FString::Printf(
				TEXT("Tokenize failed: %s"), *TokenizeError));
		}

		const uint32 NCtx = Api.llama_n_ctx(Ctx);
		if ((uint32)PromptTokens.Num() >= NCtx)
		{
			return MakeFailure(FString::Printf(
				TEXT("Prompt size %d exceeds context %u. ")
				TEXT("Use a shorter reference voice or increase ContextSize."),
				PromptTokens.Num(), NCtx));
		}

		UE_LOG(LogInoNeuTts, Log,
			TEXT("Synth: prompt=%d tokens, n_ctx=%u, max_new=%d"),
			PromptTokens.Num(), NCtx, Options.MaxNewTokens);

		// ---- 4. Reset KV cache and prefill ----
		llama_memory_t Mem = Api.llama_get_memory(Ctx);
		Api.llama_memory_clear(Mem, /*data*/ true);

		struct llama_batch PrefillBatch =
			Api.llama_batch_get_one(PromptTokens.GetData(), PromptTokens.Num());
		if (Api.llama_decode(Ctx, PrefillBatch) != 0)
		{
			return MakeFailure(TEXT("llama_decode (prefill) failed."));
		}

		// ---- 5. AR generation loop ----
		struct llama_sampler* Chain = BuildSamplerChain(Api, Options);
		ON_SCOPE_EXIT
		{
			Api.llama_sampler_free(Chain);
		};

		FString GeneratedText;
		// Generated text grows roughly 16 chars per speech token; pre-reserve.
		GeneratedText.Reserve(Options.MaxNewTokens * 16);

		bool bCancelled = false;
		const int32 GeneratedCount = RunArLoop(
			Api, Ctx, Vocab,
			Runner.GetStopTokenId(),
			Chain,
			Options.MaxNewTokens,
			CancelFlag,
			GeneratedText,
			bCancelled);

		if (bCancelled)
		{
			FInoNeuTtsResult R;
			R.bSuccess = false;
			R.ErrorMessage = TEXT("Cancelled");
			R.SampleRate = kSampleRate;
			return R;
		}

		if (GeneratedCount == 0)
		{
			return MakeFailure(
				TEXT("AR loop produced 0 tokens (model emitted EOG/stop immediately)."));
		}

		// ---- 6. Parse <|speech_N|> ids ----
		const TArray<int32> SpeechIds = ParseSpeechTokenIds(GeneratedText);
		if (SpeechIds.Num() == 0)
		{
			return MakeFailure(FString::Printf(
				TEXT("No <|speech_N|> tokens parsed from generated text (length=%d)."),
				GeneratedText.Len()));
		}

		UE_LOG(LogInoNeuTts, Log,
			TEXT("Synth: generated %d tokens, parsed %d speech ids"),
			GeneratedCount, SpeechIds.Num());

		// ---- 7. ONNX decode ----
		FInoOnnxSession* Decoder = Runner.GetDecoder();
		if (Decoder == nullptr)
		{
			return MakeFailure(TEXT("Runner has no NeuCodec decoder session."));
		}

		TArray<float> AudioFloat;
		FString DecodeError;
		if (!DecodeSpeechTokens(*Decoder, SpeechIds, AudioFloat, DecodeError))
		{
			return MakeFailure(FString::Printf(
				TEXT("NeuCodec decode failed: %s"), *DecodeError));
		}

		// ---- 8. Float -> int16 PCM bytes ----
		FInoNeuTtsResult Result;
		Result.bSuccess = true;
		Result.SampleRate = kSampleRate;
		Result.NumChannels = 1;

		UInoAudioFunctionLibrary::Float32ToInt16PcmBytesMono(
			TArrayView<const float>(AudioFloat),
			Result.AudioSamples);

		Result.DurationSeconds = static_cast<float>(AudioFloat.Num()) / kSampleRate;
		Result.GenerationTimeSeconds = static_cast<float>(FPlatformTime::Seconds() - T0);
		Result.RealTimeFactor = (Result.DurationSeconds > 0.0f)
			? (Result.GenerationTimeSeconds / Result.DurationSeconds)
			: 0.0f;

		UE_LOG(LogInoNeuTts, Log,
			TEXT("Synth ok: %.2f s audio in %.2f s (RTF %.2f)"),
			Result.DurationSeconds,
			Result.GenerationTimeSeconds,
			Result.RealTimeFactor);

		return Result;
	}
}
