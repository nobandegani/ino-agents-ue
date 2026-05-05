// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

// FInoOnnxSessionOptions — the generic ONNX session config used by every
// ORT consumer in the codebase. NeuTTS embeds it for the NeuCodec decoder.
#include "Onnx/InoOnnxTypes.h"

// FInoLlamaModelParams + FInoLlamaContextParams — the generic GGUF
// load + context config. NeuTTS embeds them for the speech LM backbone.
#include "InoLlamaTypes.h"

// FInoDownloadProgress — shared with InoNodes' generic downloader so the
// LoadModelAsync progress delegate hands callers the same struct any other
// download flow in the project uses. (BytesPerSecond, ETA, retry attempt,
// per-file + overall progress, current file name + index, etc.)
#include "InoDownloader.h"

#include "InoNeuTtsTypes.generated.h"

/**
 * Internal C++ representation of a NeuTTS reference voice — what the
 * synth pipeline (FInoNeuTtsRunner::PrimeVoice, RunSynthesis,
 * RunStreamingSynthesis) takes as input.
 *
 * NOT exposed to Blueprint. Blueprint code references voices via
 * UInoNeuTtsVoiceAsset (the .inv UAsset wrapper); the asset's
 * ToRuntimeVoice() builds one of these for the subsystem's internal
 * voice-set path.
 *
 * The fields stay UPROPERTY (without Blueprint tags) so UE's GC /
 * reflection / move semantics work correctly when the runner caches
 * a copy on FInoNeuTtsVoiceCache.
 */
USTRUCT()
struct INONEUTTSNATIVE_API FInoNeuTtsVoice
{
	GENERATED_BODY()

	/** Voice identifier, e.g. "jo", "dave". Used as the cache key. */
	UPROPERTY()
	FString Name;

	/** eSpeak language code, e.g. "en-us", "de", "fr-fr", "es". */
	UPROPERTY()
	FString Language;

	/** Transcript of the source WAV. Used as the prompt's reference text. */
	UPROPERTY()
	FString RefText;

	/**
	 * IPA phonemization of RefText. Empty = phonemize lazily via
	 * InoSpeakNG on prime. Pre-baked by build-voices.py for hot-path
	 * performance when present.
	 */
	UPROPERTY()
	FString RefPhones;

	/**
	 * NeuCodec speech tokens for the source WAV (50 Hz codes, single
	 * codebook). Identical for Nano + Air — only the LLM backbone
	 * differs between the two variants.
	 */
	UPROPERTY()
	TArray<int32> RefCodes;

	/** True iff this voice has the minimum data needed for synthesis. */
	UPROPERTY()
	bool bIsValid = false;
};

/**
 * One-time configuration for loading the NeuTTS engine. Passed to
 * UInoNeuTtsSubsystem::LoadModelAsync.
 *
 * No variant selector: Nano / Air is just metadata in the model registry
 * (DisplayName / file naming), the runtime architecture is identical for
 * both. To switch backbones, change BackboneModelName and re-load.
 */
USTRUCT(BlueprintType)
struct INONEUTTSNATIVE_API FInoNeuTtsConfig
{
	GENERATED_BODY()

	/**
	 * DisplayName of the entry to use from UInoNeuTtsNativeSettings::BackboneModels.
	 * Empty string = use the first entry in the array. Switching backbones
	 * requires UnloadModel + LoadModelAsync.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	FString BackboneModelName;

	/**
	 * DisplayName of the entry to use from
	 * UInoNeuTtsNativeSettings::DecoderModels. Empty = first entry.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	FString DecoderModelName;

	/**
	 * llama.cpp model-load options for the speech LM backbone — GPU
	 * offload count, mmap / mlock, vocab-only mode, multi-GPU split,
	 * etc. See FInoLlamaModelParams for every knob.
	 *
	 * Defaults are CPU-only with mmap on (matches the previous hardcoded
	 * path). Override Backbone.NumGpuLayers > 0 to opt into Vulkan
	 * offload on supported platforms (Win64 + Android).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts|Backbone")
	FInoLlamaModelParams Backbone;

	/**
	 * llama.cpp inference-context options for the speech LM backbone —
	 * context size, batch sizes, thread counts, flash attention, KV
	 * cache offload + dtype, etc. See FInoLlamaContextParams.
	 *
	 * NumCtx defaults to 2048 (NeuTTS's documented limit ~= 30 s of
	 * audio plus the reference voice prompt). Going lower saves RAM
	 * but caps the longest synthesizable utterance.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts|Backbone")
	FInoLlamaContextParams BackboneContext;

	/**
	 * ONNX session config for the NeuCodec decoder. Defaults are CPU-only
	 * with full graph optimization — the same configuration the previous
	 * inline path used. Override to opt into accelerators (DirectML on
	 * Windows, NNAPI / WebGPU on Android), tune thread counts, enable
	 * profiling, etc. See FInoOnnxSessionOptions for every knob.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts|Decoder")
	FInoOnnxSessionOptions DecoderOnnx;

	/**
	 * Run a tiny dummy inference on the decoder right after Create() to
	 * pay the JIT / kernel-selection / mem-pattern setup costs once at
	 * load time, off the user-visible synth hot path. Highly recommended;
	 * disable only if load-time latency matters more than first-synth
	 * jitter.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts|Decoder")
	bool bWarmupDecoderOnLoad = true;

	/**
	 * Run a tiny dummy prefill + 1-token decode on the GGUF backbone right
	 * after Create() to pay the cold-start costs (kernel selection, KV
	 * allocation, first-decode jitter) once at load time. Adds ~50–200 ms
	 * to LoadModelAsync but removes the same jitter from the user-visible
	 * first synth. Highly recommended; disable only when load-time latency
	 * matters more than first-synth jitter.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts|Backbone")
	bool bWarmupBackboneOnLoad = true;
};

/** Per-call sampling parameters for synthesis. */
USTRUCT(BlueprintType)
struct INONEUTTSNATIVE_API FInoNeuTtsOptions
{
	GENERATED_BODY()

	/**
	 * Hard cap on AR-loop iterations. Each token = 20 ms of audio at
	 * 50 Hz code rate, so 2048 tokens ≈ 40 s output. Hitting this without
	 * the stop token aborts cleanly with a warning.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	int32 MaxNewTokens = 2048;

	/**
	 * Minimum tokens to generate before honouring the stop-token / EOG
	 * checks. Prevents rare premature stops where the model emits
	 * <|SPEECH_GENERATION_END|> in the first handful of tokens. Matches
	 * the upstream torch reference's `min_new_tokens=50`. Set to 0 to
	 * disable.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	int32 MinNewTokens = 50;

	/** llama.cpp sampler temperature. Matches the upstream Python default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	float Temperature = 1.0f;

	/** llama.cpp top-k sampler. Matches the upstream Python default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	int32 TopK = 50;

	/**
	 * llama.cpp top-p (nucleus) sampler. 0.95 matches llama-cpp-python's
	 * default which the upstream NeuTTS reference relies on (it overrides
	 * only Temperature + TopK and lets TopP / MinP stay at their defaults).
	 * Set to 1.0 to disable.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	float TopP = 0.95f;

	/**
	 * llama.cpp min-p sampler. 0.05 matches llama-cpp-python's default.
	 * Set to 0.0 to disable.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	float MinP = 0.05f;

	/** Seed for the dist sampler. -1 = use a fresh time-based seed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	int32 RandomSeed = -1;
};

/** Result delivered to OnComplete after a synthesis run. */
USTRUCT(BlueprintType)
struct INONEUTTSNATIVE_API FInoNeuTtsResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "InoNeuTts")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "InoNeuTts")
	FString ErrorMessage;

	/** 24 kHz mono int16 PCM little-endian bytes. Feed to RuntimeAudioImporter. */
	UPROPERTY(BlueprintReadOnly, Category = "InoNeuTts")
	TArray<uint8> AudioSamples;

	UPROPERTY(BlueprintReadOnly, Category = "InoNeuTts")
	int32 SampleRate = 24000;

	UPROPERTY(BlueprintReadOnly, Category = "InoNeuTts")
	int32 NumChannels = 1;

	UPROPERTY(BlueprintReadOnly, Category = "InoNeuTts")
	float DurationSeconds = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "InoNeuTts")
	float GenerationTimeSeconds = 0.0f;

	/** Wall-clock GenerationTime / DurationSeconds. < 1.0 means faster than real-time. */
	UPROPERTY(BlueprintReadOnly, Category = "InoNeuTts")
	float RealTimeFactor = 0.0f;
};

// ---------------------------------------------------------------
// Delegates
//
// Subsystem methods take SINGLE-CAST delegates — Blueprint UFUNCTIONs
// don't accept multicast delegates as parameters. Async-action wrappers
// (UInoNeuTtsSynthesize) re-export these as MULTICAST BlueprintAssignable
// properties so a Blueprint exec node can fan out to OnComplete / OnError
// pins.
// ---------------------------------------------------------------

/** LoadModelAsync completion (subsystem param, single-cast). Game thread. */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FInoNeuTtsLoadedDelegate,
	bool, bSuccess, FString, ErrorMessage);

/**
 * LoadModelAsync per-tick download progress (subsystem param, single-cast).
 * Game thread. Re-uses the shared FInoDownloadProgress struct so callers see
 * the same fields any other InoNodes-driven download flow exposes — bytes
 * sent / total, per-file + overall %, current file name + index, rolling
 * BytesPerSecond + ETA, retry attempt counter.
 *
 * Fires zero or more times during LoadModelAsync's download phase (skipped
 * entirely when both files are already cached on disk), strictly before the
 * FInoNeuTtsLoadedDelegate.
 */
DECLARE_DYNAMIC_DELEGATE_OneParam(FInoNeuTtsDownloadProgressDelegate,
	const FInoDownloadProgress&, Progress);

/**
 * SetActiveVoiceAsync completion (subsystem param, single-cast). Game thread.
 *
 * bSuccess=true means the voice prefix was tokenized + prefilled into the
 * LM context AND the post-prefix KV state was snapshotted; subsequent
 * SynthesizeAsync calls reuse that snapshot rather than re-prefilling the
 * prefix on every synth.
 *
 * bSuccess=false (with ErrorMessage) means priming failed — typical reasons
 * are no model loaded, voice not valid (no ref_codes), or the staged
 * llama.cpp build doesn't expose the state_seq API. SynthesizeAsync will
 * also fail until a voice is successfully primed.
 */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FInoNeuTtsVoiceReadyDelegate,
	bool, bSuccess, FString, ErrorMessage);

/** SynthesizeAsync completion (subsystem param, single-cast). Game thread. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FInoNeuTtsSynthesisCompleteDelegate,
	const FInoNeuTtsResult&, Result);

/**
 * Streaming chunk callback (subsystem param, single-cast). Fires on the
 * game thread as the worker emits each completed audio chunk.
 *
 *   AudioChunk: 24 kHz mono int16 PCM LE bytes (variable length;
 *               typically ~24 KB per chunk for ChunkTokens=25)
 *   bIsFinal:   true on the last chunk of a synthesis (the irregular
 *               tail). OnComplete fires immediately afterwards with
 *               the concatenated full waveform.
 */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FInoNeuTtsAudioChunkDelegate,
	const TArray<uint8>&, AudioChunk, bool, bIsFinal);

/** Async-action exec-pin variant (BlueprintAssignable, multicast). Game thread. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInoNeuTtsLoaded,
	bool, bSuccess, FString, ErrorMessage);

/** Async-action exec-pin variant (BlueprintAssignable, multicast). Game thread. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInoNeuTtsSynthesisComplete,
	const FInoNeuTtsResult&, Result);

/** Async-action streaming chunk variant (BlueprintAssignable, multicast). Game thread. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInoNeuTtsAudioChunk,
	const TArray<uint8>&, AudioChunk, bool, bIsFinal);
