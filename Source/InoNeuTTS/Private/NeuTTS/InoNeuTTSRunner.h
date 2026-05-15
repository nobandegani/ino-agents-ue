// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "NeuTTS/InoNeuTTSTypes.h"  // FInoNeuTTSVoice

class FInoNeuTTSDecoderSession;
class FInoNeuTTSEngineBackend;

/**
 * Owns the loaded backbone (`FInoNeuTTSEngineBackend`) + decoder
 * (`FInoNeuTTSDecoderSession`) + the currently-primed voice cache.
 * One instance per UInoNeuTTSSubsystem load, shared across all synths.
 *
 * The voice cache holds pre-resolved RefPhones (post-eSpeak +
 * whitespace normalization) and the pre-built `<|speech_N|>` token
 * block string. These are the dominant per-synth cost when computed
 * inline; caching them on PrimeVoice saves ~10-30 ms per synth on
 * Q8 NeuTTS Nano.
 *
 * Threading: NOT internally thread-safe. The subsystem serializes
 * PrimeVoice and synth dispatch on its single ThreadPool worker so
 * concurrent mutation is impossible. Multiple synths against the
 * same primed voice are safe (the cache is read-only during synth).
 *
 * Note: NeuTTS doesn't actually reuse KV cache across synths (each
 * synth's voice + input are part of the prompt every time), so the
 * "voice cache" here is purely about avoiding redundant phonemization
 * and string-building work — not about preserving inference state.
 */
class FInoNeuTTSRunner
{
public:
    /** Load both models + optional warmups. Returns nullptr on failure.
     *
     *  Takes the full FInoNeuTTSConfig (instead of separate params) so
     *  adding new tuning knobs (Backend, ActivationType, MaxNumTokens,
     *  CacheDir, PrefillChunkSize) doesn't churn the signature on every
     *  bump. The Config's ModelName fields are unused here — the caller
     *  has already resolved them to the BackbonePath / DecoderPath args. */
    static TSharedPtr<FInoNeuTTSRunner, ESPMode::ThreadSafe> Create(
        const FString& BackbonePath,
        const FString& DecoderPath,
        const FInoNeuTTSConfig& Config,
        FString& OutError);

    ~FInoNeuTTSRunner();

    FInoNeuTTSRunner(const FInoNeuTTSRunner&) = delete;
    FInoNeuTTSRunner& operator=(const FInoNeuTTSRunner&) = delete;

    /** Phonemize the voice's RefText (if no pre-baked RefPhones),
     *  normalize whitespace, build the speech-tokens block. Result
     *  cached on the runner; subsequent synths against this voice
     *  reuse the cache. */
    bool PrimeVoice(const FInoNeuTTSVoice& Voice, FString& OutError);

    void ClearVoiceCache();
    bool HasCachedVoice(const FString& VoiceName) const;

    // Read-only accessors for the worker (called inside RunSynthesis).
    const FString& GetCachedVoiceName() const   { return CachedVoiceName; }
    const FString& GetCachedRefPhones() const   { return CachedRefPhones; }
    const FString& GetCachedSpeechBlock() const { return CachedSpeechBlock; }

    FInoNeuTTSEngineBackend*  GetEngine()  const { return Engine.Get(); }
    FInoNeuTTSDecoderSession* GetDecoder() const { return Decoder.Get(); }

private:
    FInoNeuTTSRunner() = default;

    TUniquePtr<FInoNeuTTSEngineBackend>  Engine;
    TUniquePtr<FInoNeuTTSDecoderSession> Decoder;

    FString CachedVoiceName;
    FString CachedRefPhones;
    FString CachedSpeechBlock;

    /** True iff Config.bWarmupBackboneOnLoad was set at Create time and
     *  the backbone hasn't been warmed yet. Cleared after the first
     *  successful PrimeVoice triggers Engine->WarmupForVoice. Decoder
     *  warmup is unaffected — it can warm immediately at Create time
     *  with an all-zero codes vector since it has no per-voice state. */
    bool bBackboneWarmupPending = false;
};
