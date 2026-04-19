// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Onnx/InoOnnxSession.h"

/**
 * FInoChatterboxModels — owns the three ORT sessions that make up
 * Chatterbox Turbo's runtime inference pipeline.
 *
 * The Chatterbox Turbo ONNX bundle from ResembleAI/chatterbox-turbo-ONNX
 * splits the model into four components (see Plugins/InoAgents/Chatterbox/
 * README.md for the big picture). Three are runtime; the fourth
 * (speech_encoder) is authoring-only and doesn't ship in the game:
 *
 *   language_model        T3 autoregressive backbone. Takes token
 *                         embeddings + speaker conditioning, emits
 *                         speech-token logits one step at a time.
 *   embed_tokens          Token-ID -> embedding lookup. Split out of
 *                         the LM so the embedding table can be memory-
 *                         mapped and quantized independently.
 *   conditional_decoder   S3Gen mel decoder + HiFi-GAN vocoder,
 *                         merged into a single ORT graph. Takes a
 *                         chunk of speech tokens + the speaker
 *                         embedding, emits PCM audio.
 *
 * This class is a minimal bundle that:
 *   - locates the three staged .onnx files for the requested variant
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
 *   LoadFromDir is synchronous and blocks until all three sessions
 *   are created. Suitable for calling from a worker thread; do NOT
 *   call from the game thread (model load can take 1-5 seconds on
 *   first run due to ORT graph optimization). Phase D's subsystem
 *   wraps this in an async dispatch.
 *
 * Ownership:
 *   Move-semantic TUniquePtr. Destruction releases the ORT sessions
 *   in reverse order via FInoOnnxSession's destructor.
 */
class FInoChatterboxModels
{
public:
    /**
     * Load the three runtime sessions from a staged model directory.
     *
     * Expects the directory to contain (for variant X):
     *   language_model_X.onnx        (+ .onnx_data for large variants)
     *   embed_tokens_X.onnx          (+ .onnx_data)
     *   conditional_decoder_X.onnx   (+ .onnx_data)
     *
     * The .onnx_data companion is discovered automatically by ORT
     * (the .onnx file references it by relative path via the ONNX
     * external-data protobuf convention) — no extra code needed here.
     *
     * Returns a valid TUniquePtr on success, nullptr on failure (with
     * *OutError populated and a detailed log line).
     */
    static TUniquePtr<FInoChatterboxModels> LoadFromDir(
        const FString& BaseDir,
        const FString& Variant,
        FString* OutError = nullptr);

    ~FInoChatterboxModels() = default;
    FInoChatterboxModels(const FInoChatterboxModels&) = delete;
    FInoChatterboxModels& operator=(const FInoChatterboxModels&) = delete;
    FInoChatterboxModels(FInoChatterboxModels&&) = delete;   // heap-only via TUniquePtr
    FInoChatterboxModels& operator=(FInoChatterboxModels&&) = delete;

    /** Dump each session's metadata (I/O names, shapes, dtypes, active
     *  providers) to LogInoAgents at Log level. Useful for designing
     *  the tokenizer and runners — run this once and read the log to
     *  see exactly what each session expects. */
    void LogMetadata() const;

    // --- Session accessors (non-owning pointers) ---
    // Returned pointers are valid until this FInoChatterboxModels is
    // destroyed. Consumers should NOT store them long-term.

    FInoOnnxSession* GetLanguageModel() const       { return LanguageModel.Get(); }
    FInoOnnxSession* GetEmbedTokens() const         { return EmbedTokens.Get(); }
    FInoOnnxSession* GetConditionalDecoder() const  { return ConditionalDecoder.Get(); }

    /** Which quantization variant was loaded (e.g. "fp16" or "q4f16").
     *  The same variant string is also embedded in each session's
     *  on-disk filename. */
    const FString& GetVariant() const { return Variant; }

    /** Absolute directory containing the staged .onnx files. */
    const FString& GetBaseDir() const { return BaseDir; }

private:
    FInoChatterboxModels() = default;

    TUniquePtr<FInoOnnxSession> LanguageModel;
    TUniquePtr<FInoOnnxSession> EmbedTokens;
    TUniquePtr<FInoOnnxSession> ConditionalDecoder;

    FString Variant;
    FString BaseDir;
};
