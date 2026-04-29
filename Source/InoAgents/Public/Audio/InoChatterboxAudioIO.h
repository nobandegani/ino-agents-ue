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
    INOAGENTS_API bool ReadMonoWavAsFloat32(
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
     * Internally converts to int16 bytes via Float32ToInt16PcmBytesMono
     * and delegates the WAV-header framing to WriteInt16PcmBytesAsWav
     * below, so both the float and byte entry points produce identical
     * output.
     *
     * Used by smoke tests to dump generated audio for manual listening
     * and as a debugging aid in the subsystem when SynthesizeAsync's
     * caller wants to save a waveform to disk.
     */
    INOAGENTS_API bool WriteMonoInt16Wav(
        const FString& Path,
        TArrayView<const float> Samples,
        int32 SampleRate);

    /**
     * Write a mono PCM int16 WAV file from already-int16-PCM bytes.
     *
     * Input is little-endian int16 samples packed as bytes (same shape
     * as FInoChatterboxSynthesisResult::AudioSamples after Phase D
     * Commit 4+). Byte count MUST be a multiple of 2; returns false
     * otherwise. Self-contained writer (no UE audio module dependency).
     *
     * Runs a pure memcpy into the WAV body after writing the 44-byte
     * RIFF/fmt/data header — no per-sample work, O(N) bytes total.
     */
    INOAGENTS_API bool WriteInt16PcmBytesAsWav(
        const FString& Path,
        TArrayView<const uint8> PcmBytes,
        int32 SampleRate);

    /**
     * Convert int16 PCM little-endian bytes (24 kHz mono, as passed in
     * FInoChatterboxVoice::ReferenceSamples) into float32 samples in
     * [-1, +1] for the ONNX pipeline.
     *
     * Byte count must be a multiple of 2 (int16-aligned). Returns false
     * on alignment failure, with *OutError populated if non-null;
     * OutSamples is cleared on failure. x86_64 and ARM64 are both
     * little-endian, so we can reinterpret directly without a swap.
     *
     * O(NumSamples) linear scan; ~20 µs for a 5-second reference clip.
     */
    INOAGENTS_API bool Int16PcmBytesToFloat32Mono(
        TArrayView<const uint8> PcmBytes,
        TArray<float>& OutSamples,
        FString* OutError = nullptr);

    /**
     * Convert float32 samples in [-1, +1] (or anywhere; clamped
     * internally) into int16 PCM little-endian bytes, mono.
     *
     * Output byte count is exactly 2 × Samples.Num(). Same quantization
     * policy as WriteMonoInt16Wav: Clamp(v, -1, +1) * 32767, rounded
     * to nearest. No header, no padding.
     */
    INOAGENTS_API void Float32ToInt16PcmBytesMono(
        TArrayView<const float> Samples,
        TArray<uint8>& OutBytes);

} // namespace InoChatterbox
