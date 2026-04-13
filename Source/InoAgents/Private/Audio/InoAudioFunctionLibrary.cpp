// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoAudioFunctionLibrary.h"

TArray<uint8> UInoAudioFunctionLibrary::GenerateEmptyRawAudio(
    float DurationMs, int32 SampleRate, int32 NumChannels)
{
    const float ClampedMs    = FMath::Max(DurationMs, 0.0f);
    const int32 ClampedRate  = FMath::Clamp(SampleRate, 1, 192000);
    const int32 ClampedCh    = FMath::Clamp(NumChannels, 1, 8);

    // (ms / 1000) * rate gives frames per channel; multiply by channel
    // count to get interleaved sample count; multiply by 2 (sizeof int16)
    // to get bytes. Compute in int64 to avoid int32 overflow at long
    // durations + high rates.
    const int64 NumFrames =
        static_cast<int64>(static_cast<double>(ClampedMs) / 1000.0
                           * static_cast<double>(ClampedRate));
    const int64 NumBytes  =
        NumFrames * ClampedCh * static_cast<int64>(sizeof(int16));

    TArray<uint8> Bytes;
    if (NumBytes <= 0 || NumBytes > MAX_int32)
    {
        return Bytes;
    }
    Bytes.SetNumZeroed(static_cast<int32>(NumBytes));
    return Bytes;
}
