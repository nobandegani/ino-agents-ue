// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

// FInoOnnxSessionOptions — the generic ONNX session config used by every
// ORT consumer in the codebase. NeuTTS embeds it for the NeuCodec decoder.
#include "Onnx/InoOnnxTypes.h"

#include "InoNeuTtsTypes.generated.h"

/** NeuTTS backbone variant. Both share the same NeuCodec decoder. */
UENUM(BlueprintType)
enum class EInoNeuTtsVariant : uint8
{
	Nano UMETA(DisplayName = "Nano (smaller, faster, ~195 MB)"),
	Air  UMETA(DisplayName = "Air  (larger, higher quality, ~430 MB)"),
};

/**
 * Pre-encoded reference voice for cloning.
 *
 * Produced offline by Plugins/InoAgents/NeuTTS/scripts/build-voices.py
 * from Neuphonic's vendor/samples/*.{pt,txt}. The runtime loads these
 * via the voice registry, then phonemizes RefText (lazy) into RefPhones
 * on first synth.
 */
USTRUCT(BlueprintType)
struct INONEUTTSNATIVE_API FInoNeuTtsVoice
{
	GENERATED_BODY()

	/** Voice identifier, e.g. "jo", "dave". Set from the .nvoice.json file. */
	UPROPERTY(BlueprintReadWrite, Category = "InoNeuTts")
	FString Name;

	/** eSpeak language code, e.g. "en-us", "de", "fr-fr", "es". */
	UPROPERTY(BlueprintReadWrite, Category = "InoNeuTts")
	FString Language;

	/** Transcript of the source WAV. Used as the prompt's reference text. */
	UPROPERTY(BlueprintReadWrite, Category = "InoNeuTts")
	FString RefText;

	/**
	 * IPA phonemization of RefText. Empty after voice load; filled lazily
	 * on first synth via InoSpeakNG and cached back into the voice. May
	 * also be pre-baked by an offline script for hot-path performance.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "InoNeuTts")
	FString RefPhones;

	/**
	 * NeuCodec speech tokens for the source WAV (50 Hz codes, single
	 * codebook). Identical for Nano + Air — only the LLM backbone
	 * differs between the two variants.
	 */
	UPROPERTY()
	TArray<int32> RefCodes;

	/** True iff this voice was loaded from a valid .nvoice.json file. */
	UPROPERTY(BlueprintReadOnly, Category = "InoNeuTts")
	bool bIsValid = false;
};

/**
 * One-time configuration for loading the NeuTTS engine. Passed to
 * UInoNeuTtsSubsystem::LoadModelAsync.
 */
USTRUCT(BlueprintType)
struct INONEUTTSNATIVE_API FInoNeuTtsConfig
{
	GENERATED_BODY()

	/** Backbone variant. Switching variants requires UnloadModel + LoadModelAsync. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	EInoNeuTtsVariant Variant = EInoNeuTtsVariant::Nano;

	/** llama.cpp GPU offload layer count. 0 = CPU only (recommended default). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	int32 NumGpuLayers = 0;

	/** llama.cpp thread count for prefill / decode. 0 = auto. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	int32 NumThreads = 0;

	/**
	 * Maximum context size (tokens) for the llama.cpp session. NeuTTS
	 * documents ~30 s of audio with prompt at 2048; smaller saves RAM
	 * but caps the longest synthesizable utterance.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	int32 ContextSize = 2048;

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

	/** llama.cpp sampler temperature. Matches the upstream Python default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	float Temperature = 1.0f;

	/** llama.cpp top-k sampler. Matches the upstream Python default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTts")
	int32 TopK = 50;

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
