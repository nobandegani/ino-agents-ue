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
		 * Returns the raw OrtTensor outputs in OutTensors (cleared first)
		 * so the caller can read the float buffer directly via
		 * OutTensors[0].GetData<float>() + GetElementCount(). Skipping
		 * the CopyToArray step saves one allocation + memcpy per decode
		 * — meaningful in streaming where we decode many chunks per synth.
		 *
		 * View-based input so streaming can pass a sliding window without
		 * copying its underlying SpeechIdCache.
		 */
		bool DecodeSpeechTokens(
			FInoOnnxSession& Decoder,
			TArrayView<const int32> SpeechIds,
			TArray<FInoOnnxTensor>& OutTensors,
			FString& OutError)
		{
			OutTensors.Reset();

			// Input shape [1, 1, N] int32 — Neuphonic's onnx_example.py
			// passes codes shaped like this and the model uses a single
			// input tensor.
			const int64 N = SpeechIds.Num();
			const TArray<int64> Shape = { 1, 1, N };

			FInoOnnxTensor InTensor =
				FInoOnnxTensor::CreateFromBufferCopy<int32>(Shape, SpeechIds);

			if (!InTensor.IsValid())
			{
				OutError = TEXT("Failed to build int32 input tensor for NeuCodec decoder.");
				return false;
			}

			TArray<FInoOnnxTensor> Inputs;
			Inputs.Add(MoveTemp(InTensor));

			if (!Decoder.Run(Inputs, OutTensors, &OutError))
			{
				return false;
			}

			if (OutTensors.Num() == 0 || !OutTensors[0].IsValid())
			{
				OutError = TEXT("NeuCodec decoder returned no output tensors.");
				return false;
			}
			if (OutTensors[0].GetElementCount() == 0)
			{
				OutError = TEXT("NeuCodec decoder output tensor was empty.");
				return false;
			}

			return true;
		}

		/**
		 * Linear overlap-add of N audio frames at fixed stride. Triangular
		 * windowing (peaks at frame midpoint, tapers to zero at edges)
		 * blends the overlapping regions. Port of neutts.py
		 * `_linear_overlap_add` (which is itself from facebookresearch/encodec).
		 *
		 * total_size = max over i of (stride * i + frames[i].Num())
		 *
		 * Output is the windowed sum divided by the running weight sum,
		 * so where windows overlap the audio averages cleanly to unit
		 * gain rather than summing to clipping.
		 */
		void LinearOverlapAdd(
			const TArray<TArray<float>>& Frames,
			int32 Stride,
			TArray<float>& OutSamples)
		{
			OutSamples.Reset();
			if (Frames.Num() == 0)
			{
				return;
			}

			int32 TotalSize = 0;
			for (int32 i = 0; i < Frames.Num(); ++i)
			{
				const int32 End = Stride * i + Frames[i].Num();
				if (End > TotalSize)
				{
					TotalSize = End;
				}
			}

			OutSamples.SetNumZeroed(TotalSize);
			TArray<float> SumWeight;
			SumWeight.SetNumZeroed(TotalSize);

			int32 Offset = 0;
			for (const TArray<float>& Frame : Frames)
			{
				const int32 Length = Frame.Num();
				const float Denom  = static_cast<float>(Length + 1);
				for (int32 i = 0; i < Length; ++i)
				{
					// t = linspace(0, 1, Length+2)[1:-1]
					const float T = static_cast<float>(i + 1) / Denom;
					// triangular window: 0.5 - |t - 0.5|, peaks at 0.5
					const float W = 0.5f - FMath::Abs(T - 0.5f);
					OutSamples[Offset + i] += W * Frame[i];
					SumWeight [Offset + i] += W;
				}
				Offset += Stride;
			}

			for (int32 i = 0; i < TotalSize; ++i)
			{
				if (SumWeight[i] > 0.0f)
				{
					OutSamples[i] /= SumWeight[i];
				}
			}
		}

		/**
		 * Append every <|speech_NNN|> id found in TokenText to OutIds.
		 * Most generated tokens are exactly one speech id, but the regex
		 * is tolerant of multi-piece text and (rare) non-speech text
		 * intermixed.
		 */
		void ParseAndAppendSpeechIds(const FString& TokenText, TArray<int32>& OutIds)
		{
			static const FRegexPattern Pattern(TEXT("<\\|speech_(\\d+)\\|>"));
			FRegexMatcher Matcher(Pattern, TokenText);
			while (Matcher.FindNext())
			{
				const FString IdStr = Matcher.GetCaptureGroup(1);
				OutIds.Add(FCString::Atoi(*IdStr));
			}
		}
	}

	FInoNeuTtsResult RunSynthesis(
		FInoNeuTtsRunner& Runner,
		const FString& InputText,
		const FInoNeuTtsVoice& Voice,
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

		// ---- 1. Phonemize input text + ref text (use pre-baked if present) ----
		const FString InputPhones =
			UInoSpeakNGBPLibrary::Phonemize(InputText, Voice.Language);
		if (InputPhones.IsEmpty())
		{
			return MakeFailure(FString::Printf(
				TEXT("Phonemization of input failed for language '%s'. ")
				TEXT("Is InoSpeakNG initialized?"),
				*Voice.Language));
		}

		FString RefPhones = Voice.RefPhones;
		if (RefPhones.IsEmpty())
		{
			RefPhones =
				UInoSpeakNGBPLibrary::Phonemize(Voice.RefText, Voice.Language);
			if (RefPhones.IsEmpty())
			{
				return MakeFailure(FString::Printf(
					TEXT("Phonemization of voice ref_text failed for language '%s'."),
					*Voice.Language));
			}
		}

		// ---- 2. Build prompt ----
		const FString Prompt = BuildSynthesisPrompt(
			RefPhones, InputPhones, Voice.RefCodes);

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

		TArray<FInoOnnxTensor> Outputs;
		FString DecodeError;
		if (!DecodeSpeechTokens(*Decoder, SpeechIds, Outputs, DecodeError))
		{
			return MakeFailure(FString::Printf(
				TEXT("NeuCodec decode failed: %s"), *DecodeError));
		}

		// ---- 8. Float -> int16 PCM bytes (read straight from OrtTensor) ----
		const float* AudioFloat = Outputs[0].GetData<float>();
		const int32  NumSamples = static_cast<int32>(Outputs[0].GetElementCount());

		FInoNeuTtsResult Result;
		Result.bSuccess     = true;
		Result.SampleRate   = kSampleRate;
		Result.NumChannels  = 1;

		UInoAudioFunctionLibrary::Float32ToInt16PcmBytesMono(
			TArrayView<const float>(AudioFloat, NumSamples),
			Result.AudioSamples);

		Result.DurationSeconds = static_cast<float>(NumSamples) / kSampleRate;
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

	// ====================================================================
	//  Streaming variant
	// ====================================================================

	FInoNeuTtsResult RunStreamingSynthesis(
		FInoNeuTtsRunner& Runner,
		const FString& InputText,
		const FInoNeuTtsVoice& Voice,
		const FInoNeuTtsOptions& Options,
		int32 ChunkTokens,
		const FInoNeuTtsStreamCallbacks& Callbacks,
		const std::atomic<bool>* CancelFlag)
	{
		const double T0 = FPlatformTime::Seconds();

		// Streaming chunking constants — match neutts.py.
		constexpr int32 kOverlapFrames = 1;
		constexpr int32 kLookforward   = 5;
		constexpr int32 kLookback      = 50;

		// Default to the Python reference's frames_per_chunk on <=0.
		if (ChunkTokens <= 0)
		{
			ChunkTokens = 25;
		}
		const int32 StrideSamples = ChunkTokens * kCodecHopLength;

		// ----- Setup (mirrors RunSynthesis up to + including prefill) -----
		if (InputText.IsEmpty())
		{
			return MakeFailure(TEXT("InputText is empty."));
		}
		if (!Voice.bIsValid || Voice.RefCodes.Num() == 0)
		{
			return MakeFailure(TEXT("Voice has no ref_codes."));
		}

		const auto& Api = Runner.GetLlamaApi();
		const struct llama_vocab* Vocab = Runner.GetVocab();
		struct llama_context* Ctx = Runner.GetContext();
		FInoOnnxSession* Decoder  = Runner.GetDecoder();
		if (Decoder == nullptr)
		{
			return MakeFailure(TEXT("Runner has no NeuCodec decoder session."));
		}

		const FString InputPhones =
			UInoSpeakNGBPLibrary::Phonemize(InputText, Voice.Language);
		if (InputPhones.IsEmpty())
		{
			return MakeFailure(FString::Printf(
				TEXT("Phonemization of input failed for language '%s'."),
				*Voice.Language));
		}

		FString RefPhones = Voice.RefPhones;
		if (RefPhones.IsEmpty())
		{
			RefPhones = UInoSpeakNGBPLibrary::Phonemize(Voice.RefText, Voice.Language);
			if (RefPhones.IsEmpty())
			{
				return MakeFailure(TEXT("Phonemization of voice ref_text failed."));
			}
		}

		const FString Prompt = BuildSynthesisPrompt(
			RefPhones, InputPhones, Voice.RefCodes);

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
				TEXT("Prompt size %d exceeds context %u."),
				PromptTokens.Num(), NCtx));
		}

		UE_LOG(LogInoNeuTts, Log,
			TEXT("Stream synth: prompt=%d tokens, n_ctx=%u, max_new=%d, chunk=%d"),
			PromptTokens.Num(), NCtx, Options.MaxNewTokens, ChunkTokens);

		llama_memory_t Mem = Api.llama_get_memory(Ctx);
		Api.llama_memory_clear(Mem, /*data*/ true);

		struct llama_batch PrefillBatch =
			Api.llama_batch_get_one(PromptTokens.GetData(), PromptTokens.Num());
		if (Api.llama_decode(Ctx, PrefillBatch) != 0)
		{
			return MakeFailure(TEXT("llama_decode (prefill) failed."));
		}

		struct llama_sampler* Chain = BuildSamplerChain(Api, Options);
		ON_SCOPE_EXIT
		{
			Api.llama_sampler_free(Chain);
		};

		// ----- Streaming AR loop with rolling decode + overlap-add -----

		// Speech-id buffer: starts populated with the reference voice
		// codes (which the LM will continue from). NDecodedTokens tracks
		// the right edge of "samples already emitted to the caller".
		TArray<int32> SpeechIdCache = Voice.RefCodes;
		int32 NDecodedTokens  = SpeechIdCache.Num();
		int32 NDecodedSamples = 0;

		TArray<TArray<float>> AudioCache;          // each entry is one cropped chunk
		TArray<uint8>         AccumulatedBytes;    // for OnComplete's full waveform

		auto EmitChunk = [&](TArray<float>&& FloatSlice, bool bIsFinal)
		{
			TArray<uint8> Bytes;
			UInoAudioFunctionLibrary::Float32ToInt16PcmBytesMono(FloatSlice, Bytes);

			// Append to the rolling full-waveform buffer for OnComplete.
			AccumulatedBytes.Append(Bytes);

			if (Callbacks.OnChunk)
			{
				Callbacks.OnChunk(MoveTemp(Bytes), bIsFinal);
			}
		};

		bool bCancelled = false;
		FString GenText;  // buffer for parsing token pieces (kept small per step)

		for (int32 Iter = 0; Iter < Options.MaxNewTokens; ++Iter)
		{
			if (CancelFlag != nullptr &&
				(Iter % kCancelCheckEvery) == 0 &&
				CancelFlag->load(std::memory_order_relaxed))
			{
				bCancelled = true;
				break;
			}

			llama_token Next = Api.llama_sampler_sample(Chain, Ctx, -1);

			if (Next == Runner.GetStopTokenId() ||
				Api.llama_vocab_is_eog(Vocab, Next))
			{
				break;
			}

			// Capture this token's text and parse out any speech ids.
			GenText.Reset();
			AppendTokenPiece(Api, Vocab, Next, GenText);
			Api.llama_sampler_accept(Chain, Next);
			ParseAndAppendSpeechIds(GenText, SpeechIdCache);

			struct llama_batch StepBatch = Api.llama_batch_get_one(&Next, 1);
			if (Api.llama_decode(Ctx, StepBatch) != 0)
			{
				UE_LOG(LogInoNeuTts, Error,
					TEXT("llama_decode failed at AR step %d."), Iter);
				break;
			}

			// Have we accumulated enough new tokens to fill the decode
			// window? The window goes
			// [..., NDecodedTokens + ChunkTokens + kLookforward + kOverlapFrames),
			// so we need that many tokens past NDecodedTokens before we
			// can read it. Python's reference uses just ChunkTokens +
			// kLookforward as the threshold and relies on list slicing
			// to silently clamp short reads — C++ TArrayView reads past
			// the end into uninitialized memory. Match the actual window
			// extent here.
			const int32 NewSinceEmit = SpeechIdCache.Num() - NDecodedTokens;
			if (NewSinceEmit < ChunkTokens + kLookforward + kOverlapFrames)
			{
				continue;
			}

			// Decode a window: lookback + (this chunk's tokens) + lookforward,
			// padded by overlap on each side.
			const int32 TokensStart = FMath::Max(
				NDecodedTokens - kLookback - kOverlapFrames, 0);
			const int32 TokensEnd =
				NDecodedTokens + ChunkTokens + kLookforward + kOverlapFrames;

			TArrayView<const int32> Window(
				SpeechIdCache.GetData() + TokensStart,
				TokensEnd - TokensStart);

			TArray<FInoOnnxTensor> ChunkOutputs;
			FString DecodeErr;
			if (!DecodeSpeechTokens(*Decoder, Window, ChunkOutputs, DecodeErr))
			{
				return MakeFailure(FString::Printf(
					TEXT("NeuCodec decode failed mid-stream: %s"), *DecodeErr));
			}

			// Crop out the non-context middle: skip the lookback prefix,
			// keep ChunkTokens + 2*overlap frames worth of samples. Reads
			// the float buffer straight off the OrtTensor (no intermediate
			// TArray<float> copy).
			const float* ChunkAudio = ChunkOutputs[0].GetData<float>();
			const int32 SampleStart =
				(NDecodedTokens - TokensStart) * kCodecHopLength;
			const int32 SampleLen   =
				(ChunkTokens + 2 * kOverlapFrames) * kCodecHopLength;

			TArray<float> Cropped;
			Cropped.Append(ChunkAudio + SampleStart, SampleLen);
			AudioCache.Add(MoveTemp(Cropped));

			// Re-run overlap-add over all chunks and slice the new portion.
			TArray<float> Combined;
			LinearOverlapAdd(AudioCache, StrideSamples, Combined);

			const int32 NewSamplesEnd = AudioCache.Num() * StrideSamples;
			const int32 SliceLen = FMath::Max(0, NewSamplesEnd - NDecodedSamples);

			TArray<float> NewSlice;
			NewSlice.Append(Combined.GetData() + NDecodedSamples, SliceLen);
			EmitChunk(MoveTemp(NewSlice), /*bIsFinal*/ false);

			NDecodedSamples = NewSamplesEnd;
			NDecodedTokens += ChunkTokens;
		}

		if (bCancelled)
		{
			FInoNeuTtsResult R;
			R.bSuccess     = false;
			R.ErrorMessage = TEXT("Cancelled");
			R.SampleRate   = kSampleRate;
			return R;
		}

		// ----- Final irregular chunk (remaining tokens past the last emit) -----
		if (SpeechIdCache.Num() > NDecodedTokens)
		{
			const int32 RemainingTokens = SpeechIdCache.Num() - NDecodedTokens;

			const int32 TokensStart = FMath::Max(
				SpeechIdCache.Num() - (kLookback + kOverlapFrames + RemainingTokens),
				0);

			const int32 RawSampleStart =
				(SpeechIdCache.Num() - TokensStart - RemainingTokens - kOverlapFrames)
				* kCodecHopLength;
			const int32 SampleStart = FMath::Max(0, RawSampleStart);

			TArrayView<const int32> Window(
				SpeechIdCache.GetData() + TokensStart,
				SpeechIdCache.Num() - TokensStart);

			TArray<FInoOnnxTensor> ChunkOutputs;
			FString DecodeErr;
			if (!DecodeSpeechTokens(*Decoder, Window, ChunkOutputs, DecodeErr))
			{
				return MakeFailure(FString::Printf(
					TEXT("NeuCodec decode failed on final chunk: %s"), *DecodeErr));
			}

			const float* ChunkAudio        = ChunkOutputs[0].GetData<float>();
			const int32  NumChunkSamples   =
				static_cast<int32>(ChunkOutputs[0].GetElementCount());
			const int32  NumCropped        = FMath::Max(0, NumChunkSamples - SampleStart);

			TArray<float> Cropped;
			Cropped.Append(ChunkAudio + SampleStart, NumCropped);
			AudioCache.Add(MoveTemp(Cropped));

			TArray<float> Combined;
			LinearOverlapAdd(AudioCache, StrideSamples, Combined);

			const int32 RemainingNum =
				FMath::Max(0, Combined.Num() - NDecodedSamples);

			TArray<float> FinalSlice;
			FinalSlice.Append(Combined.GetData() + NDecodedSamples, RemainingNum);
			EmitChunk(MoveTemp(FinalSlice), /*bIsFinal*/ true);
		}
		else if (AudioCache.Num() > 0)
		{
			// AR loop ended exactly on a chunk boundary (rare). Make sure
			// the consumer sees a final-marked chunk so it knows we're done.
			EmitChunk(TArray<float>{}, /*bIsFinal*/ true);
		}

		FInoNeuTtsResult Result;
		Result.bSuccess     = true;
		Result.SampleRate   = kSampleRate;
		Result.NumChannels  = 1;
		Result.AudioSamples = MoveTemp(AccumulatedBytes);
		Result.DurationSeconds =
			static_cast<float>(Result.AudioSamples.Num() / 2) / kSampleRate;
		Result.GenerationTimeSeconds =
			static_cast<float>(FPlatformTime::Seconds() - T0);
		Result.RealTimeFactor = (Result.DurationSeconds > 0.0f)
			? (Result.GenerationTimeSeconds / Result.DurationSeconds)
			: 0.0f;

		UE_LOG(LogInoNeuTts, Log,
			TEXT("Stream synth ok: %.2f s audio, %d chunks, %.2f s wall (RTF %.2f)"),
			Result.DurationSeconds,
			AudioCache.Num(),
			Result.GenerationTimeSeconds,
			Result.RealTimeFactor);

		return Result;
	}
}
