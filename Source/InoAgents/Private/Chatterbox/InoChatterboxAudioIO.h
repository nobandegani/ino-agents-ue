// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

namespace InoChatterbox
{
    /**
     * Sample rate every Chatterbox Turbo stage operates at, both input
     * (reference audio fed to speech_encoder) and output (decoder
     * waveform). Hardcoded by the model — not a tunable.
     */
    constexpr int32 kSampleRate = 24000;

    /**
     * Read a mono WAV file from disk as float32 in [-1, +1].
     *
     * Accepted formats (strict — see the extensive reasoning in
     * InoChatterboxAudioIO.cpp):
     *
     *   AudioFormat=1 (PCM)        + BitsPerSample=16   → int16 scaled by 1/32768
     *   AudioFormat=3 (IEEE float) + BitsPerSample=32   → memcpy, already [-1, +1]
     *
     * Channel count MUST be 1. Sample rate is reported via
     * OutSampleRate but NOT validated — the caller decides whether a
     * mismatch vs kSampleRate is a warning or a hard error.
     * UInoChatterboxTtsSubsystem::SynthesizeAsync errors hard on
     * mismatch; smoke tests warn and feed through.
     *
     * Tolerant of extra 'LIST' / 'fact' / 'JUNK' chunks between the
     * fmt and data chunks (common for AudioFormat=3).
     *
     * Returns false on any parse / IO failure, writing a human-readable
     * message to *OutError when provided. OutSamples is cleared on
     * failure so partial reads never leak upward.
     */
    bool ReadMonoWavAsFloat32(
        const FString& Path,
        TArray<float>& OutSamples,
        int32& OutSampleRate,
        FString* OutError = nullptr);

    /**
     * Write a mono PCM int16 WAV file at the given sample rate.
     *
     * Samples outside [-1, +1] are clamped before quantization. Returns
     * false on write failure. Self-contained WAV writer — doesn't
     * require the UE audio module to be initialized.
     *
     * Used by smoke tests to dump generated audio for manual listening
     * and as a debugging aid in the subsystem when SynthesizeAsync's
     * caller wants to save a waveform to disk.
     */
    bool WriteMonoInt16Wav(
        const FString& Path,
        TArrayView<const float> Samples,
        int32 SampleRate);

} // namespace InoChatterbox
