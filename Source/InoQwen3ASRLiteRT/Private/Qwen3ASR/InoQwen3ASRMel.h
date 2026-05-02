// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

/**
 * Whisper-style log-mel spectrogram for Qwen3-ASR audio preprocessing.
 *
 * Converts a mono float32 waveform at kSampleRate into a (kNMels, kMelFrames)
 * log-mel spectrogram matching exactly what Qwen3-ASR's WhisperFeatureExtractor
 * (preprocessor_config.json) produces.
 *
 * Algorithm:
 *   1. Center-pad audio with kNFft/2 reflected samples on each side
 *   2. Frame into windows of kNFft samples, hopping by kHopLength
 *   3. Apply periodic Hann window
 *   4. Compute magnitude-squared spectrum (DFT, |X|^2)
 *   5. Project through 128-bin Slaney-style mel filterbank
 *   6. log10 with floor 1e-10
 *   7. Whisper normalization: clamp to [max-8, max], (x+4)/4 → ~[0, 1]
 *
 * Output shape: (kNMels, kMelFrames) flat row-major float32.
 *
 * Performance note: this implementation uses a naive DFT (O(N^2) per frame).
 * For our 5-second 80,000-sample windows that's ~320 M ops, ~50–300 ms on
 * typical desktop CPUs. Plenty fast for Phase 2 and dwarfed by the 64-step
 * decoder loop. Swap in a mixed-radix FFT later if it becomes a bottleneck.
 */
class FInoQwen3ASRMel
{
public:
    FInoQwen3ASRMel();

    /**
     * Compute log-mel spectrogram from a 16 kHz mono float32 waveform.
     *
     * @param Audio       Input samples; must contain at least kAudioWindowSamples
     *                    elements. If shorter, the trailing positions are
     *                    treated as zero-padded silence. If longer, only the
     *                    first kAudioWindowSamples are used.
     * @param OutLogMel   Resized to kNMels * kMelFrames floats, row-major
     *                    [mel_bin][frame].
     */
    void Compute(TArrayView<const float> Audio, TArray<float>& OutLogMel) const;

    /** Last computed min/max log-mel values, for diagnostic logging. */
    void GetLastValueRange(float& OutMin, float& OutMax) const
    {
        OutMin = LastMin;
        OutMax = LastMax;
    }

private:
    // Precomputed at construction.
    TArray<float> HannWindow;            // size kNFft
    TArray<float> MelFilterbank;         // (kNMels, kNFft/2 + 1) row-major

    // Diagnostic state from the most recent Compute() call.
    mutable float LastMin = 0.0f;
    mutable float LastMax = 0.0f;

    /** Build the kNMels × (kNFft/2+1) Slaney-style mel filterbank in place. */
    void BuildMelFilterbank();

    /** Build a kNFft-sized periodic Hann window. */
    void BuildHannWindow();
};
