// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

// Pulls in FInoDownloadProgress for the download-progress delegate below.
// InoNodes is a PUBLIC dep of this module so consumers binding to the
// delegate get the struct definition for free.
#include "InoDownloader.h"

#include "InoNeuTTSTypes.generated.h"

// ============================================================================
// Runtime voice — INTERNAL C++ struct, not Blueprint-exposed.
// ============================================================================
//
// Voice data flows: `.inv` JSON (or programmatic NewObject) →
// UInoNeuTTSVoiceAsset (UAsset wrapper, Blueprint-friendly) →
// UInoNeuTTSVoiceAsset::ToRuntimeVoice() returns FInoNeuTTSVoice → handed
// to FInoNeuTTSRunner for PrimeVoice / synth.
//
// The asset is the Blueprint touch-point; this struct is the runner-side
// representation. Keeping the asset and the runtime struct distinct lets
// the runner be game-thread-free (no UObject access) while the asset
// stays Blueprint-discoverable.
//
USTRUCT()
struct FInoNeuTTSVoice
{
    GENERATED_BODY()

    /** Voice identifier, used as the cache key by FInoNeuTTSRunner. */
    UPROPERTY()
    FString Name;

    /** eSpeak language code ("en-us", "de", "fr-fr", "es", ...). */
    UPROPERTY()
    FString Language = TEXT("en-us");

    /** Transcript of the reference WAV. Phonemized at runtime via
     *  InoSpeakNG when RefPhones is empty. */
    UPROPERTY()
    FString RefText;

    /** Optional pre-baked IPA phonemization of RefText. Set by the
     *  offline encoder; if empty, runtime phonemization fires per synth. */
    UPROPERTY()
    FString RefPhones;

    /** NeuCodec FSQ codes (50 Hz, single codebook). Produced by the
     *  offline encoder. Each value is in [0, 65535]; the
     *  `<|speech_N|>` tokens fed to the backbone are these ids. */
    UPROPERTY()
    TArray<int32> RefCodes;

    /** True if Name + Language + RefCodes are all set. Mirrors
     *  UInoNeuTTSVoiceAsset::IsUsable(). */
    UPROPERTY()
    bool bIsValid = false;
};

// ============================================================================
// Model registry entries — Project Settings → Plugins → InoNeuTTS.
// ============================================================================

/**
 * One entry in the backbone (.litertlm) registry. Maps a display name +
 * on-disk filename to a download URL so the subsystem can auto-download
 * on first use. Same shape as InoLiteRtLm's model entry.
 *
 * File lands at `<FPaths::ProjectPersistentDownloadDir()>/InoAgents/NeuTTS/<LocalFileName>`.
 * ExpectedSha256 (when set) is verified after download AND against any
 * existing cached file, so a corrupt cached file is re-downloaded
 * automatically.
 */
USTRUCT(BlueprintType)
struct FInoNeuTTSBackboneEntry
{
    GENERATED_BODY()

    /** Human-readable name shown in editor / Blueprint pickers. Also the
     *  lookup key for FInoNeuTTSConfig::BackboneModelName, case-insensitive
     *  (falls back to LocalFileName match if nothing matches by name). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    FString DisplayName;

    /** Direct GET URL (HuggingFace `.../resolve/main/...`, S3, your CDN).
     *  Public URLs only — no auth handling. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    FString DownloadUrl;

    /** Filename to save as locally. Must end in `.litertlm` for LiteRT-LM
     *  to recognize it. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    FString LocalFileName;

    /** Hex-encoded SHA-256 hash for integrity verification. Optional but
     *  strongly recommended. Lowercase, 64 chars, no separators —
     *  same format as `sha256sum` / Hugging Face LFS OIDs. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model",
              meta = (DisplayName = "Expected SHA-256"))
    FString ExpectedSha256;

    /** Total file size in bytes. Used for the download-progress percentage
     *  when the server doesn't return Content-Length on GET. 0 = HEAD-probe
     *  before downloading. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    int64 FileSizeBytes = 0;

    /** eSpeak language code the model was trained on / is intended for
     *  ("en-us", "de", "multi", ...). Informational. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    FString Language = TEXT("en-us");

    /** Quantization label. Informational only — the actual dtype is
     *  intrinsic to the .litertlm file. Common values: "Q4_BLOCK32",
     *  "Q8", "FP16", "FP32". */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    FString Quantization;
};

/**
 * One entry in the decoder (.tflite) registry. Same shape as the
 * backbone entry, minus the language field — NeuCodec is
 * language-agnostic (operates on FSQ codes, not text).
 *
 * File lands at the same `<persistent>/InoAgents/NeuTTS/<LocalFileName>`
 * directory as backbones.
 */
USTRUCT(BlueprintType)
struct FInoNeuTTSDecoderEntry
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    FString DisplayName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    FString DownloadUrl;

    /** Must end in `.tflite`. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    FString LocalFileName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model",
              meta = (DisplayName = "Expected SHA-256"))
    FString ExpectedSha256;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Config, Category = "Model")
    int64 FileSizeBytes = 0;
};

// ============================================================================
// Config + Options + Result — passed to LoadModelAsync / SynthesizeAsync.
// ============================================================================

/**
 * Configuration for loading a backbone + decoder pair. Passed to
 * UInoNeuTTSSubsystem::LoadModelAsync / DownloadModelAsync.
 *
 * The model selection fields are matched (case-insensitive) against
 * UInoNeuTTSSettings::BackboneModels / DecoderModels — DisplayName first,
 * LocalFileName as fallback. Empty string = pick the first entry in the
 * corresponding array.
 */
USTRUCT(BlueprintType)
struct FInoNeuTTSConfig
{
    GENERATED_BODY()

    /** Backbone DisplayName (or LocalFileName). Empty = first BackboneModels entry. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTTS")
    FString BackboneModelName;

    /** Decoder DisplayName (or LocalFileName). Empty = first DecoderModels entry. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTTS")
    FString DecoderModelName;

    /** Drive a tiny dummy generate through the backbone at load time to
     *  pay JIT / kernel-selection / KV allocation jitter once. Removes
     *  the same jitter from the first user-visible synth. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTTS|Warmup")
    bool bWarmupBackboneOnLoad = true;

    /** Drive a tiny dummy decode through NeuCodec at load time to pay
     *  XNNPACK delegate setup once. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTTS|Warmup")
    bool bWarmupDecoderOnLoad = true;
};

/**
 * Per-synthesis options. Defaults match Neuphonic's reference inference
 * loop (test_tts.py) and the values baked into the .litertlm bundle.
 *
 * NOTE: Temperature and TopK are currently informational. The .litertlm
 * bundle bakes sampler_params (TOP_P with k=50, T=1.0), and overriding
 * those via LiteRT-LM's `litert_lm_session_config_set_sampler_params`
 * setter triggers a vendor regression in the current build
 * (CPU sampler returns Unimplemented for any non-TOP_P type — see
 * Phase 0b notes in our CLAUDE.md). Production code passes NULL session
 * config so the engine uses the bundle's baked sampler. The fields are
 * retained here for forward compatibility when the upstream regression
 * is fixed.
 */
USTRUCT(BlueprintType)
struct FInoNeuTTSOptions
{
    GENERATED_BODY()

    /** Hard cap on AR-loop iterations. Default 2048 (matches the
     *  bundle's max_num_tokens). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTTS",
              meta = (ClampMin = "1"))
    int32 MaxNewTokens = 2048;

    /** Skip stop-token checks until at least this many tokens emitted.
     *  Mirrors Neuphonic's torch reference path's `min_new_tokens=50`
     *  guard against rare premature stops on short reference voices. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTTS",
              meta = (ClampMin = "0"))
    int32 MinNewTokens = 50;

    /** Temperature. Currently informational (see struct docstring). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTTS|Sampling",
              meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float Temperature = 1.0f;

    /** Top-K. Currently informational (see struct docstring). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTTS|Sampling",
              meta = (ClampMin = "1"))
    int32 TopK = 50;

    /** RNG seed. Currently informational. <0 = non-deterministic. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoNeuTTS|Sampling")
    int32 RandomSeed = -1;
};

/**
 * Result of one synthesis call. Carries both success and failure cases
 * so async-action wrappers can fan to OnComplete/OnError using the same
 * payload type.
 *
 * On success: AudioSamples is 24 kHz mono int16 PCM little-endian bytes,
 * ready to feed into UStreamingSoundWave::AppendAudioDataFromRAW
 * (RuntimeAudioImporter) or written to disk via
 * UInoAudioFunctionLibrary::SaveInt16PcmAsWav.
 */
USTRUCT(BlueprintType)
struct FInoNeuTTSResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "InoNeuTTS")
    bool bSuccess = false;

    UPROPERTY(BlueprintReadOnly, Category = "InoNeuTTS")
    FString ErrorMessage;

    /** 24 kHz mono int16 PCM little-endian. */
    UPROPERTY(BlueprintReadOnly, Category = "InoNeuTTS")
    TArray<uint8> AudioSamples;

    UPROPERTY(BlueprintReadOnly, Category = "InoNeuTTS")
    int32 SampleRate = 24000;

    UPROPERTY(BlueprintReadOnly, Category = "InoNeuTTS")
    int32 NumChannels = 1;

    UPROPERTY(BlueprintReadOnly, Category = "InoNeuTTS")
    float DurationSeconds = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "InoNeuTTS")
    float GenerationTimeSeconds = 0.0f;

    /** GenerationTimeSeconds / DurationSeconds. <1.0 means
     *  faster-than-real-time (good); >1.0 means slower. */
    UPROPERTY(BlueprintReadOnly, Category = "InoNeuTTS")
    float RealTimeFactor = 0.0f;
};

// ============================================================================
// Delegates — single-cast for subsystem method args; multicast for
// async-action wrapper pins.
// ============================================================================

/** Fired once by LoadModelAsync / DownloadModelAsync at terminal completion. */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FInoNeuTTSLoadedDelegate,
    bool, bSuccess,
    FString, ErrorMessage);

/** Fired 0+ times during download. Empty when files are already cached. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FInoNeuTTSDownloadProgressDelegate,
    const FInoDownloadProgress&, Progress);

/** Fired once by SetActiveVoiceAsync at terminal completion. */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FInoNeuTTSVoiceReadyDelegate,
    bool, bSuccess,
    FString, ErrorMessage);

/** Fired once by SynthesizeAsync / SynthesizeStreamAsync at terminal completion. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FInoNeuTTSSynthesisCompleteDelegate,
    const FInoNeuTTSResult&, Result);

/** Fired per streaming-chunk emit. `bIsFinal=true` on the last chunk. */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FInoNeuTTSAudioChunkDelegate,
    const TArray<uint8>&, AudioChunk,
    bool, bIsFinal);

// Multicast variants used by the BlueprintAsyncActionBase wrappers
// (`UInoNeuTTSSynthesize` / `UInoNeuTTSStreamSynthesize`). Async actions
// expose multicast `BlueprintAssignable` UPROPERTYs as exec pins, so the
// delegate parameter NAMES below become the pin names in the Blueprint
// graph — keep them readable.

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInoNeuTTSSynthesisComplete,
    const FInoNeuTTSResult&, Result);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInoNeuTTSAudioChunk,
    const TArray<uint8>&, AudioChunk,
    bool, bIsFinal);
