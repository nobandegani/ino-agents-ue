// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Atomic.h"
#include "Templates/Function.h"

class FInoChatterboxTurboNativeModels;
class FInoChatterboxTurboNativeTokenizer;

/**
 * FInoChatterboxTurboNativeRunner — one-utterance synthesis driver.
 *
 * Wraps the full Chatterbox Turbo inference pipeline (speech_encoder →
 * embed_tokens → language_model AR loop → conditional_decoder) into a
 * single entry point: text + reference voice in, PCM waveform out.
 * The runner is a faithful port of Resemble AI's official reference
 * script at https://huggingface.co/ResembleAI/chatterbox-turbo-ONNX —
 * see the full step-by-step correspondence in
 * Plugins/InoAgents/CLAUDE.md → "Chatterbox Turbo TTS (first planned
 * ONNX consumer)".
 *
 * This class does NOT own the models or tokenizer — it borrows them
 * from a caller-provided FInoChatterboxTurboNativeModels + FInoChatterboxTurboNativeTokenizer
 * pair. That means:
 *   - Cheap to construct. No session loads here.
 *   - Multiple runners may share the same bundle (though each Synthesize
 *     call itself is single-threaded; parallel synthesis needs separate
 *     bundles or external locking).
 *   - Caller owns the lifetime; the runner must outlive neither.
 *
 * Threading:
 *   SynthesizeText blocks for the duration of the pipeline (hundreds of
 *   milliseconds to multiple seconds depending on utterance length and
 *   target hardware). Call it from a ThreadPool worker or an Async task,
 *   never the game thread.
 *
 * Phase D's UInoChatterboxTurboNativeSubsystem will wrap this class to provide
 * a Blueprint-friendly async API; all the synchronous guts live here.
 */
class FInoChatterboxTurboNativeRunner
{
public:
    /**
     * Parameters controlling the AR generation loop.
     *
     * Defaults match Resemble AI's published reference script. Change
     * with care — higher MaxNewTokens means longer utterances at the
     * cost of more per-call latency; RepetitionPenalty values outside
     * the ~1.1–1.3 range degrade prosody.
     */
    struct FSynthesisOptions
    {
        /** Upper bound on speech tokens generated before the loop
         *  force-stops. Typical real utterances fit in 256–512 tokens;
         *  long narration may need the full 1024. */
        int32 MaxNewTokens = 1024;

        /** Divisor applied to already-seen token logits so the model
         *  stops repeating itself. 1.0 = disabled. Reference uses 1.2. */
        float RepetitionPenalty = 1.2f;
    };

    /**
     * Result of one successful SynthesizeText call.
     *
     * AudioSamples is 24 kHz mono float32 in the range roughly [-1, 1],
     * ready to be quantised to int16 for WAV output or fed to a
     * streaming audio consumer.
     */
    struct FSynthesisResult
    {
        /** Waveform samples, float32 at SampleRate Hz, mono. */
        TArray<float> AudioSamples;

        /** Always 24000 for Chatterbox Turbo — included here so callers
         *  don't have to hardcode the rate when handing the buffer off. */
        int32 SampleRate = 24000;

        /** How many speech tokens the AR loop actually produced before
         *  hitting STOP or max_new_tokens. Includes neither the leading
         *  START_SPEECH_TOKEN nor the trailing STOP (if any). */
        int32 NumGeneratedTokens = 0;

        /** True if the loop terminated on STOP_SPEECH_TOKEN, false if it
         *  ran out at MaxNewTokens. STOP is the normal termination;
         *  false means the utterance may have been truncated. */
        bool bHitStopToken = false;

        /** Wall-clock time from start of SynthesizeText to return. */
        double TotalElapsedMs = 0.0;

        /** Sub-stage timings for diagnostics. */
        double EncoderMs       = 0.0;
        double EmbedTotalMs    = 0.0;
        double LanguageModelMs = 0.0;
        double DecoderMs       = 0.0;
    };

    /**
     * Construct a runner bound to the given bundle + tokenizer.
     *
     * Both references must outlive the runner and must have been
     * initialised successfully (bundle LoadFromDir returned a valid
     * TUniquePtr; tokenizer LoadFromJson succeeded).
     */
    FInoChatterboxTurboNativeRunner(
        const FInoChatterboxTurboNativeModels&   InModels,
        const FInoChatterboxTurboNativeTokenizer& InTokenizer);

    FInoChatterboxTurboNativeRunner(const FInoChatterboxTurboNativeRunner&) = delete;
    FInoChatterboxTurboNativeRunner& operator=(const FInoChatterboxTurboNativeRunner&) = delete;

    /**
     * Streaming audio-chunk callback signature.
     *
     * Fired from SynthesizeText's worker thread while synthesis is
     * still running, once per "chunk" (see StreamChunkTokens on
     * SynthesizeText below). The first argument is the NEW float32
     * samples produced since the last call — not the full accumulated
     * waveform. Samples are 24 kHz mono in the range roughly [-1, 1],
     * same format as FSynthesisResult::AudioSamples.
     *
     * NumGeneratedTokens is the count of speech tokens the AR loop has
     * produced at the point the chunk was decoded (excludes the leading
     * START marker). Monotonically increasing across calls within one
     * SynthesizeText invocation. Useful for progress UI.
     *
     * bIsFinal is true on exactly one call per synthesis — the final
     * chunk, which includes the trailing 3× silence token padding and
     * corresponds to the waveform stored in FSynthesisResult::AudioSamples.
     * After that call returns, no more callbacks will fire for this
     * SynthesizeText invocation.
     *
     * Firing rules:
     *   - OnChunk unset:                        no calls, use OutResult.
     *   - OnChunk set, StreamChunkTokens == 0:  one call at end (bIsFinal=true)
     *                                           with the full waveform.
     *   - OnChunk set, StreamChunkTokens  > 0:  one call every N AR tokens
     *                                           (bIsFinal=false), then one
     *                                           final call (bIsFinal=true).
     *
     * The callback runs on the worker thread, same thread SynthesizeText
     * is executing on. Keep it short — do the audio-dispatch bookkeeping
     * and return. Game-thread hops should happen inside the callback
     * target (via AsyncTask or similar), not block here.
     *
     * The TArrayView's backing storage is owned by the runner and
     * becomes invalid as soon as the callback returns. Copy out any
     * data you need to persist.
     *
     * IMPORTANT — decoder cost of streaming:
     *   Each intermediate chunk re-runs the conditional_decoder on the
     *   full generated prefix so far. Decoder cost is roughly O(T²) in
     *   speech tokens (full attention). So total decoder work scales
     *   as O(K³ / N) where K is the final token count and N is
     *   StreamChunkTokens. Rough rules of thumb for K ≈ 200 tokens:
     *     N=20   → ~30× more decoder work than single-shot
     *     N=50   → ~13× more
     *     N=100  → ~7×  more
     *   Total synth wallclock grows less than this (the LM work is
     *   unchanged), but expect 2–4× slower total time at N=20 vs.
     *   single-shot. The tradeoff: first-audio latency drops from
     *   "full synth done" to "one chunk done" (~5–10× lower).
     */
    using FOnStreamChunk = TFunction<void(
        TArrayView<const float> NewSamples,
        int32                   NumGeneratedTokens,
        bool                    bIsFinal)>;

    /**
     * Synthesize one utterance.
     *
     * @param Text             Text to voice. May include paralinguistic
     *                         tags like "[laugh]", "[cough]", etc. —
     *                         those are native to the Turbo tokenizer.
     * @param ReferenceAudio   Reference WAV samples at 24 kHz mono
     *                         float32. Drives the voice cloning —
     *                         speech_encoder consumes this once to
     *                         produce the conditioning tensors. For
     *                         persistent voices (same speaker across
     *                         many utterances), Phase E authoring
     *                         will precompute and cache these tensors
     *                         so this step becomes a .bin file load.
     * @param Options          Generation parameters.
     * @param OutResult        Populated with waveform + diagnostics.
     * @param OutError         On failure, filled with a human-readable
     *                         message. Optional.
     * @param Cancel           Optional cooperative-cancel flag sampled
     *                         once per AR iteration. If it flips to
     *                         true mid-synth, the next iteration early-
     *                         returns false with OutError="SynthesizeText:
     *                         cancelled" — the decoder does NOT run, so
     *                         OutResult.AudioSamples is left empty. The
     *                         caller (typically UInoChatterboxTurboNativeSubsystem)
     *                         owns the atomic and keeps it alive for the
     *                         call's duration; pass nullptr if
     *                         cancellation is not wired up. Checked with
     *                         relaxed memory order — the synth is not
     *                         publishing data through this flag, it's
     *                         just an eventually-consistent "stop please"
     *                         signal.
     * @param StreamChunkTokens Opt-in streaming cadence. When > 0, the
     *                         runner re-runs the conditional_decoder on
     *                         the accumulated speech tokens every N
     *                         generated AR tokens and fires OnChunk with
     *                         the incremental new samples. Value of 0
     *                         (the default) disables streaming — the
     *                         decoder runs only once at the end. Common
     *                         choices: 20 (a few hundred ms of audio per
     *                         chunk) for quick first-audio playback, or
     *                         higher for fewer-but-bigger chunks. Small
     *                         values trade decoder CPU for lower first-
     *                         audio latency — the decoder becomes O(N²)
     *                         in tokens (each chunk re-decodes the full
     *                         prefix), but the decoder is cheap so for
     *                         utterances under ~500 tokens the wallclock
     *                         cost is typically 10-30 % over single-shot.
     *                         Ignored if OnChunk is not set.
     * @param OnChunk          Optional callback fired on the worker
     *                         thread as new audio becomes available. See
     *                         FOnStreamChunk's doc for the callback
     *                         contract. Called AT LEAST once (the final
     *                         chunk with bIsFinal=true) on successful
     *                         return, regardless of StreamChunkTokens —
     *                         so consumers can bind this ONE callback
     *                         and get either streaming-plus-final or
     *                         just-final behaviour based on whether
     *                         StreamChunkTokens is zero or non-zero.
     *                         Not called on failure / cancellation.
     * @return                 True on success.
     *
     * Never call from the game thread; this is a synchronous blocking
     * call that takes seconds on a typical CPU.
     */
    bool SynthesizeText(
        const FString&              Text,
        TArrayView<const float>     ReferenceAudio,
        const FSynthesisOptions&    Options,
        FSynthesisResult&           OutResult,
        FString*                    OutError          = nullptr,
        const TAtomic<bool>*        Cancel            = nullptr,
        int32                       StreamChunkTokens = 0,
        const FOnStreamChunk&       OnChunk           = FOnStreamChunk()) const;

private:
    const FInoChatterboxTurboNativeModels&    Models;
    const FInoChatterboxTurboNativeTokenizer& Tokenizer;
};
