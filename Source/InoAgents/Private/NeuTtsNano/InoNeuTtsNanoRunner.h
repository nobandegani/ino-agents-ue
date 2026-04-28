// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "NeuTtsNano/InoNeuTtsNanoTypes.h"   // FInoNeuTtsNanoModelConfig

// Opaque forward-decls so this header doesn't pull in llama.h /
// ggml-backend.h. The .cpp includes InoLlama.h (from the sibling
// InoLlama plugin) which provides the real definitions via its own
// #include of llama.h.
struct llama_model;
struct llama_context;

class FInoOnnxSession;

/**
 * Per-load native-resource owner for NeuTTS Nano.
 *
 * Created by UInoNeuTtsNanoSubsystem's async load worker on a
 * ThreadPool thread after both model files are on disk. Owns:
 *
 *   - llama_model*   — the Qwen2-derived GGUF backbone. Freed in dtor
 *                      via llama_model_free (vtable).
 *   - llama_context* — the inference context with sized KV-cache.
 *                      Freed via llama_free (vtable).
 *   - FInoOnnxSession — the NeuCodec ONNX decoder (783 MB fp32).
 *                      Freed via TUniquePtr dtor.
 *   - StopTokenId    — cached token id for "<|SPEECH_GENERATION_END|>",
 *                      resolved by scanning the vocab at Create time so
 *                      the hot path doesn't re-scan on every synthesis.
 *
 * Non-copyable, non-movable — owns naked C pointers freed by the dtor,
 * and moving would require careful tracking of the llama.cpp vtable
 * pointer across instances. The subsystem holds one instance in a
 * TUniquePtr and hands a raw borrow to the SynthesisWorker.
 *
 * Thread-safety:
 *   - Create() must run off the game thread. It performs ~1 s of
 *     llama_model_load_from_file + ~2-5 s of ORT graph optimisation.
 *   - After Create, the SynthesisWorker serialises all consumer calls
 *     through one dedicated thread — neither llama_context nor
 *     FInoOnnxSession is thread-safe for concurrent inference on the
 *     same handle.
 *   - The dtor can run on any thread; freeing the handles is a C-API
 *     call with no thread affinity, and FInoOnnxSession's dtor is
 *     likewise thread-safe.
 */
class FInoNeuTtsNanoRunner
{
public:
    /**
     * Blocking constructor — loads the backbone GGUF, creates a
     * llama_context, opens the NeuCodec ORT session, and caches the
     * stop-token id. Returns nullptr on any failure; OutError carries
     * a human-readable diagnostic.
     *
     * MUST NOT be called on the game thread — heavy work.
     *
     * @param BackboneGgufPath  Absolute path to neutts-nano-*.gguf
     * @param CodecOnnxPath     Absolute path to NeuCodec model.onnx
     * @param Config            Variant + n_gpu_layers + n_ctx knobs
     * @param OutError          Set to a diagnostic on failure
     */
    static TUniquePtr<FInoNeuTtsNanoRunner> Create(
        const FString& BackboneGgufPath,
        const FString& CodecOnnxPath,
        const FInoNeuTtsNanoModelConfig& Config,
        FString& OutError);

    ~FInoNeuTtsNanoRunner();

    FInoNeuTtsNanoRunner(const FInoNeuTtsNanoRunner&) = delete;
    FInoNeuTtsNanoRunner& operator=(const FInoNeuTtsNanoRunner&) = delete;
    FInoNeuTtsNanoRunner(FInoNeuTtsNanoRunner&&) = delete;
    FInoNeuTtsNanoRunner& operator=(FInoNeuTtsNanoRunner&&) = delete;

    // Accessors (raw borrows — owner semantics stay with this Runner).
    struct llama_model*   GetModel()   const { return Model; }
    struct llama_context* GetContext() const { return Context; }
    FInoOnnxSession*      GetCodecSession() const { return CodecSession.Get(); }

    /** Token id for "<|SPEECH_GENERATION_END|>", or -1 if not found.
     *  SynthesisWorker (Milestone 4) stops sampling when the decoder
     *  emits this token, falling back to llama_vocab_is_eog + the
     *  MaxNewTokens cap when StopTokenId < 0. */
    int32 GetStopTokenId() const { return StopTokenId; }

private:
    FInoNeuTtsNanoRunner() = default;

    struct llama_model*   Model   = nullptr;
    struct llama_context* Context = nullptr;
    TUniquePtr<FInoOnnxSession> CodecSession;
    int32 StopTokenId = -1;
};
