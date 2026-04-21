// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class FInoChatterboxModels;
class FInoChatterboxTokenizer;

/**
 * FInoChatterboxRunner — one-utterance synthesis driver.
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
 * from a caller-provided FInoChatterboxModels + FInoChatterboxTokenizer
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
 * Phase D's UInoChatterboxTtsSubsystem will wrap this class to provide
 * a Blueprint-friendly async API; all the synchronous guts live here.
 */
class FInoChatterboxRunner
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
    FInoChatterboxRunner(
        const FInoChatterboxModels&   InModels,
        const FInoChatterboxTokenizer& InTokenizer);

    FInoChatterboxRunner(const FInoChatterboxRunner&) = delete;
    FInoChatterboxRunner& operator=(const FInoChatterboxRunner&) = delete;

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
        FString*                    OutError = nullptr) const;

private:
    const FInoChatterboxModels&    Models;
    const FInoChatterboxTokenizer& Tokenizer;
};
