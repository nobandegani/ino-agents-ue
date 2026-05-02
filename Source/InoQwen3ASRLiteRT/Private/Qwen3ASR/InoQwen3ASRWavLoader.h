// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

/**
 * Minimal WAV loader for Qwen3-ASR's expected input format.
 *
 * Accepts:
 *   - 16 kHz mono PCM int16 little-endian (the most common WAV encoding) →
 *     converted to float32 in [-1, 1]
 *   - 16 kHz mono PCM float32 little-endian → passed through
 *
 * Other sample rates / channel counts are rejected with a clear error log.
 * Phase 4 (the proper audio loader) will add resampling + downmix; until
 * then callers pre-convert with ffmpeg or similar.
 */
namespace InoQwen3ASR
{
    /** Load a WAV file from disk into 16 kHz mono float32 samples. */
    bool LoadWav16kMonoFromDisk(const FString& WavPath, TArray<float>& OutSamples);

    /** Decode raw WAV bytes (full file contents) into 16 kHz mono float32 samples. */
    bool LoadWav16kMonoFromBytes(TArrayView<const uint8> Bytes, TArray<float>& OutSamples);
}
