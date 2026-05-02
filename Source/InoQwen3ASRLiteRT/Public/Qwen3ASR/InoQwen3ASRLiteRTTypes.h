// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "InoQwen3ASRLiteRTTypes.generated.h"

/**
 * Configuration passed to UInoQwen3ASRLiteRTSubsystem::LoadModelAsync.
 *
 * Both file names are looked up relative to the InoAgents plugin's
 * Qwen3ASR/LiteRT/ directory:
 *   {Plugin}/Qwen3ASR/LiteRT/models/<ModelFileName>
 *   {Plugin}/Qwen3ASR/LiteRT/tokenizer/<VocabFileName>
 *
 * Phase 6 (download flow) will add support for resolving from
 * PersistentDownloadDir as well so packaged builds can fetch on first use.
 */
USTRUCT(BlueprintType)
struct INOQWEN3ASRLITERT_API FInoQwen3ASRConfig
{
    GENERATED_BODY()

    /** TFLite model filename. Default = the int8-quantized 5-second window export. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Qwen3 ASR")
    FString ModelFileName = TEXT("qwen3_asr_0.6b_5s_i8.tflite");

    /** Tokenizer vocab filename. Must match the model — Qwen3 BPE vocab.json from the base repo. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Qwen3 ASR")
    FString VocabFileName = TEXT("vocab.json");
};

/**
 * Result of a single TranscribeAudioAsync / TranscribeWavFileAsync call.
 * Returned via FOnInoQwen3ASRTranscribeComplete on the game thread.
 */
USTRUCT(BlueprintType)
struct INOQWEN3ASRLITERT_API FInoQwen3ASRTranscribeResult
{
    GENERATED_BODY()

    /** Transcribed text with the model's auto-detected language tag stripped. */
    UPROPERTY(BlueprintReadOnly, Category = "Qwen3 ASR")
    FString Text;

    /**
     * Language name as the model reported it ("English", "Chinese",
     * "Japanese", etc.). Empty if the language tag could not be parsed.
     * The model auto-detects from a list of 30 supported languages
     * (see config.json's "support_languages" field upstream).
     */
    UPROPERTY(BlueprintReadOnly, Category = "Qwen3 ASR")
    FString DetectedLanguage;

    /** Number of tokens the decoder emitted before EOS / max-len. */
    UPROPERTY(BlueprintReadOnly, Category = "Qwen3 ASR")
    int32 NumGeneratedTokens = 0;

    /** Wall-time of the mel-spectrogram step (seconds). */
    UPROPERTY(BlueprintReadOnly, Category = "Qwen3 ASR")
    float MelSeconds = 0.0f;

    /** Wall-time of the encoder pass (seconds). */
    UPROPERTY(BlueprintReadOnly, Category = "Qwen3 ASR")
    float EncodeSeconds = 0.0f;

    /** Wall-time of the autoregressive decode loop (seconds). */
    UPROPERTY(BlueprintReadOnly, Category = "Qwen3 ASR")
    float DecodeSeconds = 0.0f;

    /** Total wall-time including audio prep + tokenization (seconds). */
    UPROPERTY(BlueprintReadOnly, Category = "Qwen3 ASR")
    float TotalSeconds = 0.0f;

    /** Raw token IDs from the decoder, in emission order. Includes the language prefix. */
    UPROPERTY(BlueprintReadOnly, Category = "Qwen3 ASR")
    TArray<int32> RawTokenIds;
};

/**
 * Fired exactly once from LoadModelAsync, on the game thread.
 * @param bSuccess        true if both model and tokenizer loaded.
 * @param ErrorMessage    empty on success; human-readable diagnostic on failure.
 */
DECLARE_DYNAMIC_DELEGATE_TwoParams(
    FOnInoQwen3ASRModelLoaded,
    bool, bSuccess,
    FString, ErrorMessage);

/**
 * Fired exactly once per TranscribeAudioAsync / TranscribeWavFileAsync call,
 * on the game thread.
 * @param bSuccess        true if the model produced text without erroring.
 * @param Result          structured result (text + diagnostics). Default-init on failure.
 * @param ErrorMessage    empty on success; human-readable diagnostic on failure.
 */
DECLARE_DYNAMIC_DELEGATE_ThreeParams(
    FOnInoQwen3ASRTranscribeComplete,
    bool, bSuccess,
    FInoQwen3ASRTranscribeResult, Result,
    FString, ErrorMessage);
