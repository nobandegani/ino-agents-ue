// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "NeuTTS/InoNeuTTSTypes.h"  // EInoNeuTTSBackend, EInoNeuTTSActivationType

// Opaque LiteRT-LM engine handle, forward-declared.
extern "C" { struct LiteRtLmEngine; }

/**
 * LiteRT-LM engine wrapper for the NeuTTS Nano backbone (`.litertlm`).
 *
 * Owns one `LiteRtLmEngine*` for the lifetime of the wrapper. Each
 * synth call creates a fresh `LiteRtLmSession`, prefills the
 * hand-built prompt, decodes (blocking), parses speech IDs from the
 * response text, then deletes the session. We deliberately do NOT
 * keep sessions alive across synths because:
 *
 *   1. LiteRT-LM enforces a single-session-per-engine invariant
 *      (`occupied_executors_->contains(executor)` check in
 *      `SessionBasic::Create`).
 *   2. NeuTTS doesn't reuse KV cache across calls — each synth's
 *      voice + input context is independent and lives entirely
 *      inside the prompt.
 *
 * Sessions are configured with `apply_prompt_template=false`
 * (we hand-build the full prompt — see InoNeuTTSPromptBuilder) and
 * an optional `max_output_tokens` cap. We deliberately do NOT set
 * sampler_params — that triggers the TOP_K CPU-sampler regression
 * documented in Phase 0b. The bundle's baked sampler (TOP_P) is used.
 */
class FInoNeuTTSEngineBackend
{
public:
    /** Load `.litertlm`, create engine. Returns nullptr on failure;
     *  OutError describes what went wrong.
     *
     *  @param ModelPath        Absolute path to `<name>.litertlm`.
     *  @param Backend          Hardware accelerator ("cpu" / "gpu" / "npu").
     *  @param ActivationType   F32 / F16 / I16 / I8 activation precision.
     *  @param MaxNumTokens     Engine token budget (KV cache size). 0 =
     *                          use the value baked into the bundle.
     *  @param CacheDir         Custom XNNPACK cache dir. Empty = engine
     *                          default (alongside the model file).
     *  @param PrefillChunkSize CPU-backend prefill chunk size. 0 =
     *                          engine default. */
    static TUniquePtr<FInoNeuTTSEngineBackend> Create(
        const FString& ModelPath,
        EInoNeuTTSBackend Backend,
        EInoNeuTTSActivationType ActivationType,
        int32 MaxNumTokens,
        const FString& CacheDir,
        int32 PrefillChunkSize,
        FString& OutError);

    ~FInoNeuTTSEngineBackend();

    FInoNeuTTSEngineBackend(const FInoNeuTTSEngineBackend&) = delete;
    FInoNeuTTSEngineBackend& operator=(const FInoNeuTTSEngineBackend&) = delete;

    /**
     * One-shot synth.
     *
     *   1. Create session (with `apply_prompt_template=false`, optional
     *      `max_output_tokens=MaxNewTokens`).
     *   2. Run prefill with `FullPrompt` (text).
     *   3. Run decode (blocking).
     *   4. Regex-parse `<|speech_(\d+)|>` from the response text.
     *      The captured digit IS the FSQ code (no shift — the
     *      `<|speech_N|>` token name encodes the FSQ index `N`).
     *   5. Delete session.
     *
     * `CancelCheck` is polled (called as `() -> bool`) before prefill
     * and before decode. If true at either point, the call aborts
     * with `OutError = "Cancelled"`. Mid-decode cancellation is not
     * yet supported — caller may wait up to the full decode duration
     * (typically a few seconds for ≤200 tokens on CPU).
     */
    bool RunSynthesis(
        const FString& FullPrompt,
        int32 MaxNewTokens,
        TArray<int32>& OutSpeechIds,
        FString& OutError,
        TFunction<bool()> CancelCheck = nullptr);

    /** Tiny dummy synth (MaxNewTokens=1, near-empty prompt) to pay
     *  engine warmup cost at load time. */
    bool Warmup(FString& OutError);

private:
    FInoNeuTTSEngineBackend() = default;
    bool Initialize(
        const FString& ModelPath,
        EInoNeuTTSBackend Backend,
        EInoNeuTTSActivationType ActivationType,
        int32 MaxNumTokens,
        const FString& CacheDir,
        int32 PrefillChunkSize,
        FString& OutError);

    LiteRtLmEngine* Engine = nullptr;
};
