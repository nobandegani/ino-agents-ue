// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "InoQwen3ASRLiteRTEnv.h"
#include "InoQwen3ASRLiteRTModel.h"
#include "InoQwen3ASRLiteRTTensor.h"
#include "InoQwen3ASRMel.h"
#include "InoQwen3ASRTokenizer.h"

/** Diagnostic stats from a single Transcribe() invocation. */
struct FInoQwen3ASRTranscribeStats
{
    int32 NumGeneratedTokens = 0;
    double MelSeconds = 0.0;
    double EncodeSeconds = 0.0;
    double DecodeSeconds = 0.0;
    double TotalSeconds = 0.0;
};

/**
 * Orchestrates one inference of Qwen3-ASR-0.6B's `_5s_*` LiteRT export.
 *
 *   audio (16 kHz mono float32, ≤ 5 s)
 *      → Whisper log-mel spectrogram      (FInoQwen3ASRMel)
 *      → encoder pass                     (LiteRT signature 'encode')
 *      → autoregressive decode loop       (LiteRT signature 'decode',
 *                                          re-runs the full 64-token graph
 *                                          per step — no exposed KV cache)
 *      → token IDs
 *      → text                             (FInoQwen3ASRTokenizer)
 *
 * Move-only. Owns the compiled model (loads ~794 MB i8 weights) and the
 * tokenizer (~3 MB). LoadModel() and LoadTokenizer() are independent —
 * both must succeed before Transcribe() works.
 *
 * Decoder-start strategy: Phase 2 starts each decode loop with an empty
 * input_ids buffer (all pad, mask all zero). The model emits its first
 * token from `logits[:, 0, :]`. If that produces gibberish in practice
 * (this Qwen3 ASR export may need a chat-template prefix), the start
 * sequence becomes a configurable parameter in a follow-up.
 */
class FInoQwen3ASRRunner
{
public:
    FInoQwen3ASRRunner() = default;
    FInoQwen3ASRRunner(const FInoQwen3ASRRunner&) = delete;
    FInoQwen3ASRRunner& operator=(const FInoQwen3ASRRunner&) = delete;

    /**
     * Load the .tflite via the shared environment singleton. Caches signature
     * indices and validates the I/O shapes match the constants we hardcoded.
     */
    bool LoadModel(const FString& TfliteAbsolutePath);

    /** Load and parse vocab.json. Independent of LoadModel(). */
    bool LoadTokenizer(const FString& VocabJsonAbsolutePath);

    bool IsModelLoaded() const     { return bModelLoaded; }
    bool IsTokenizerLoaded() const { return Tokenizer.IsLoaded(); }
    bool IsReady() const           { return IsModelLoaded() && IsTokenizerLoaded(); }

    /** Read-only tokenizer access for callers that want to decode arbitrary token slices. */
    const FInoQwen3ASRTokenizer& GetTokenizer() const { return Tokenizer; }

    /**
     * Run the full audio→text pipeline on a single audio window.
     *
     * @param Audio       Mono float32 PCM samples at kSampleRate. Must contain
     *                    at least one sample; padded/truncated to kAudioWindowSamples.
     * @param OutText     UTF-8 text from the decoded token sequence.
     * @param OutTokenIds (Optional) raw generated token IDs (excluding any
     *                    leading prefix). Useful for debugging.
     * @param OutStats    (Optional) per-stage timing breakdown.
     */
    bool Transcribe(
        TArrayView<const float> Audio,
        FString& OutText,
        TArray<int32>* OutTokenIds = nullptr,
        FInoQwen3ASRTranscribeStats* OutStats = nullptr);

private:
    bool bModelLoaded = false;

    FInoQwen3ASRLiteRTModel Model;
    FInoQwen3ASRMel Mel;
    FInoQwen3ASRTokenizer Tokenizer;

    // Cached signature indices set in LoadModel().
    int32 EncodeSigIndex = INDEX_NONE;
    int32 DecodeSigIndex = INDEX_NONE;
};
