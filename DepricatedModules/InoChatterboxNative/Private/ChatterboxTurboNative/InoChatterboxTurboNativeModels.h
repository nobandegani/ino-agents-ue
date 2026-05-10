// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Onnx/InoOnnxSession.h"

#include "ChatterboxTurboNative/InoChatterboxTurboNativeTypes.h"

/**
 * FInoChatterboxTurboNativeModels — owns the four ORT sessions that make up
 * Chatterbox Turbo's runtime inference pipeline, matching the official
 * Resemble AI reference script at ResembleAI/chatterbox-turbo-ONNX.
 *
 *   speech_encoder        Reference-audio -> (cond_emb, prompt_token,
 *                         speaker_embeddings, speaker_features). Runs
 *                         once per voice at load time; the four outputs
 *                         are then reused for every utterance. Required
 *                         at runtime for voice cloning.
 *   embed_tokens          Token-ID -> embedding lookup. Split out of
 *                         the LM so the embedding table can be memory-
 *                         mapped and quantized independently.
 *   language_model        T3 autoregressive backbone. Takes token
 *                         embeddings + speaker conditioning, emits
 *                         speech-token logits one step at a time.
 *   conditional_decoder   S3Gen mel decoder + HiFi-GAN vocoder,
 *                         merged into a single ORT graph. Takes a
 *                         chunk of speech tokens + the speaker
 *                         embedding, emits PCM audio.
 *
 * This class is a minimal bundle that:
 *   - locates the four staged .onnx files for the requested variant
 *   - constructs an FInoOnnxSession for each with Chatterbox-tuned
 *     provider + optimization settings
 *   - provides LogMetadata() so the first smoke test can dump actual
 *     I/O shapes — we need those to design the AR loop and decoder
 *     wrappers in Phase B2 / B3 without guessing.
 *
 * What this class is NOT (yet):
 *   - no tokenizer (Phase B2)
 *   - no inference loop (Phase C)
 *   - no speaker-embedding application (Phase C)
 *   - no Blueprint exposure (Phase D)
 *
 * Threading:
 *   LoadFromDir is synchronous and blocks until all four sessions
 *   are created. Suitable for calling from a worker thread; do NOT
 *   call from the game thread (model load can take 1-5 seconds on
 *   first run due to ORT graph optimization). Phase D's subsystem
 *   wraps this in an async dispatch.
 *
 * Ownership:
 *   Move-semantic TUniquePtr. Destruction releases the ORT sessions
 *   in reverse order via FInoOnnxSession's destructor.
 */
class FInoChatterboxTurboNativeModels
{
public:
    /**
     * Load the four runtime sessions from a staged model directory.
     *
     * Expects the directory to contain (for variant X):
     *   speech_encoder_X.onnx        (+ .onnx_data for large variants)
     *   embed_tokens_X.onnx          (+ .onnx_data)
     *   language_model_X.onnx        (+ .onnx_data)
     *   conditional_decoder_X.onnx   (+ .onnx_data)
     *
     * The .onnx_data companion is discovered automatically by ORT
     * (the .onnx file references it by relative path via the ONNX
     * external-data protobuf convention) — no extra code needed here.
     *
     * Returns a valid TUniquePtr on success, nullptr on failure (with
     * *OutError populated and a detailed log line).
     */
    /**
     * Single-variant shorthand: load all 4 sessions at the same
     * quantization level from one directory. Used by the subsystem's
     * non-per-session path and by the smoke tests.
     */
    static TUniquePtr<FInoChatterboxTurboNativeModels> LoadFromDir(
        const FString& BaseDir,
        const FString& Variant,
        FString* OutError = nullptr,
        const FInoChatterboxTurboNativePerformanceOptions& Performance = FInoChatterboxTurboNativePerformanceOptions{});

    /**
     * Per-session load: each of the 4 sessions has its own variant +
     * directory. Used when FInoChatterboxTurboNativeModelConfig::bUsePerSessionVariants
     * is true. Consumers typically call ChatterboxResolveVariantDir
     * per session to compute the directories, then pass them here.
     *
     * Same failure semantics as LoadFromDir — returns nullptr + fills
     * OutError on first load failure (stops at the failing session).
     */
    struct FSessionLoadSpec
    {
        FString Dir;
        FString Variant;  ///< lowercase, matches the filename suffix
    };

    static TUniquePtr<FInoChatterboxTurboNativeModels> LoadPerSession(
        const FSessionLoadSpec& SpeechEncoder,
        const FSessionLoadSpec& EmbedTokens,
        const FSessionLoadSpec& LanguageModel,
        const FSessionLoadSpec& ConditionalDecoder,
        FString* OutError = nullptr,
        const FInoChatterboxTurboNativePerformanceOptions& Performance = FInoChatterboxTurboNativePerformanceOptions{});

    ~FInoChatterboxTurboNativeModels() = default;
    FInoChatterboxTurboNativeModels(const FInoChatterboxTurboNativeModels&) = delete;
    FInoChatterboxTurboNativeModels& operator=(const FInoChatterboxTurboNativeModels&) = delete;
    FInoChatterboxTurboNativeModels(FInoChatterboxTurboNativeModels&&) = delete;   // heap-only via TUniquePtr
    FInoChatterboxTurboNativeModels& operator=(FInoChatterboxTurboNativeModels&&) = delete;

    /** Dump each session's metadata (I/O names, shapes, dtypes, active
     *  providers) to LogInoAgents at Log level. Useful for designing
     *  the tokenizer and runners — run this once and read the log to
     *  see exactly what each session expects. */
    void LogMetadata() const;

    // --- Session accessors (non-owning pointers) ---
    // Returned pointers are valid until this FInoChatterboxTurboNativeModels is
    // destroyed. Consumers should NOT store them long-term.

    FInoOnnxSession* GetSpeechEncoder() const       { return SpeechEncoder.Get(); }
    FInoOnnxSession* GetEmbedTokens() const         { return EmbedTokens.Get(); }
    FInoOnnxSession* GetLanguageModel() const       { return LanguageModel.Get(); }
    FInoOnnxSession* GetConditionalDecoder() const  { return ConditionalDecoder.Get(); }

    /** Which quantization variant was loaded (e.g. "q4f16" or "fp16").
     *  The same variant string is also embedded in each session's
     *  on-disk filename. */
    const FString& GetVariant() const { return Variant; }

    /** Absolute directory containing the staged .onnx files. */
    const FString& GetBaseDir() const { return BaseDir; }

private:
    FInoChatterboxTurboNativeModels() = default;

    TUniquePtr<FInoOnnxSession> SpeechEncoder;
    TUniquePtr<FInoOnnxSession> EmbedTokens;
    TUniquePtr<FInoOnnxSession> LanguageModel;
    TUniquePtr<FInoOnnxSession> ConditionalDecoder;

    FString Variant;
    FString BaseDir;
};
