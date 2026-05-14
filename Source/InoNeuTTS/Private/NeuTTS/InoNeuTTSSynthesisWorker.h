// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include <atomic>

#include "NeuTTS/InoNeuTTSTypes.h"  // FInoNeuTTSVoice / Options / Result

class FInoNeuTTSRunner;

namespace InoNeuTTSNative
{
    /** Streaming chunk callback. AudioChunk is 24 kHz mono int16 PCM LE
     *  bytes; `bIsFinal=true` on the final emit. In the MVP a single
     *  call is made with `bIsFinal=true` carrying the whole waveform. */
    using FStreamChunkFn = TFunction<void(const TArray<uint8>& AudioChunk, bool bIsFinal)>;

    /**
     * One-shot synthesis pipeline.
     *
     *   1. Ensure the voice is primed on the runner (PrimeVoice inline
     *      if the cached name doesn't match Voice.Name).
     *   2. Phonemize the input text via InoSpeakNG.
     *   3. Build the full hand-crafted prompt
     *      (prefix + refphones + " " + inputphones + suffix + speech_block).
     *   4. Drive the backbone (FInoNeuTTSEngineBackend::RunSynthesis)
     *      to produce a list of FSQ codes.
     *   5. Decode via FInoNeuTTSDecoderSession into a 24 kHz float32
     *      waveform (trimmed to (N-1) * 480 samples).
     *   6. Convert float32 → int16 PCM LE bytes, populate
     *      FInoNeuTTSResult with timing.
     *
     * CancelFlag is polled at synth-phase boundaries (start, post-prefill,
     * post-backbone, post-decode). Mid-decode cancellation is not yet
     * supported — see FInoNeuTTSEngineBackend for details.
     *
     * Returns `Result.bSuccess == true` on success; on failure,
     * `Result.ErrorMessage` describes the error.
     */
    FInoNeuTTSResult RunSynthesis(
        FInoNeuTTSRunner* Runner,
        const FString& InputText,
        const FInoNeuTTSVoice& Voice,
        const FInoNeuTTSOptions& Options,
        TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CancelFlag);

    /**
     * MVP streaming: runs RunSynthesis under the hood, then emits the
     * full PCM result as a single chunk with `bIsFinal=true` via
     * `OnChunk`. Equivalent to one-shot synthesis with a single
     * trailing chunk.
     *
     * Future iteration will implement vendor's
     * `_infer_stream_ggml` chunked-decode-with-overlap-add pattern
     * (test_tts.py reference) for sub-second first-audio latency on
     * Q8 NeuTTS Nano. See CLAUDE.md "Roadmap" for the deferred design.
     */
    FInoNeuTTSResult RunStreamingSynthesis(
        FInoNeuTTSRunner* Runner,
        const FString& InputText,
        const FInoNeuTTSVoice& Voice,
        const FInoNeuTTSOptions& Options,
        int32 ChunkTokens,  // ignored in MVP
        const FStreamChunkFn& OnChunk,
        TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CancelFlag);
}
