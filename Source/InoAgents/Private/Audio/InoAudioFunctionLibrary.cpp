// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoAudioFunctionLibrary.h"

namespace
{
    /** Bytes per interleaved sample for a given raw format. 0 for
     *  unknown / invalid. */
    int32 BytesPerSampleForFormat(ERuntimeRAWAudioFormat Format)
    {
        switch (Format)
        {
            case ERuntimeRAWAudioFormat::Int8:
            case ERuntimeRAWAudioFormat::UInt8:    return 1;
            case ERuntimeRAWAudioFormat::Int16:
            case ERuntimeRAWAudioFormat::UInt16:   return 2;
            case ERuntimeRAWAudioFormat::Int32:
            case ERuntimeRAWAudioFormat::UInt32:
            case ERuntimeRAWAudioFormat::Float32:  return 4;
        }
        return 0;
    }
}

TArray<uint8> UInoAudioFunctionLibrary::GenerateEmptyRawAudio(
    float DurationMs, int32 SampleRate, int32 NumChannels,
    ERuntimeRAWAudioFormat Format)
{
    const float ClampedMs   = FMath::Max(DurationMs, 0.0f);
    const int32 ClampedRate = FMath::Clamp(SampleRate, 1, 192000);
    const int32 ClampedCh   = FMath::Clamp(NumChannels, 1, 8);

    const int32 BytesPerSample = BytesPerSampleForFormat(Format);
    if (BytesPerSample <= 0)
    {
        return {};
    }

    // Compute byte count in int64 to avoid int32 overflow at long
    // durations / high rates / multi-byte formats.
    const int64 NumFrames =
        static_cast<int64>(static_cast<double>(ClampedMs) / 1000.0
                           * static_cast<double>(ClampedRate));
    const int64 NumSamples = NumFrames * ClampedCh;
    const int64 NumBytes   = NumSamples * BytesPerSample;
    if (NumBytes <= 0 || NumBytes > MAX_int32)
    {
        return {};
    }

    TArray<uint8> Bytes;
    Bytes.SetNumUninitialized(static_cast<int32>(NumBytes));

    // Fill with the format's silence value:
    //   - Signed int + Float32 → all-zero bytes.
    //   - Unsigned int → midpoint per sample (0x80, 0x8000, 0x80000000).
    switch (Format)
    {
        case ERuntimeRAWAudioFormat::Int8:
        case ERuntimeRAWAudioFormat::Int16:
        case ERuntimeRAWAudioFormat::Int32:
        case ERuntimeRAWAudioFormat::Float32:
        {
            FMemory::Memset(Bytes.GetData(), 0, static_cast<SIZE_T>(NumBytes));
            break;
        }

        case ERuntimeRAWAudioFormat::UInt8:
        {
            // Single-byte midpoint = 128 = 0x80. Every byte is one
            // sample, so memset the whole buffer.
            FMemory::Memset(Bytes.GetData(), 0x80, static_cast<SIZE_T>(NumBytes));
            break;
        }

        case ERuntimeRAWAudioFormat::UInt16:
        {
            uint16* Out = reinterpret_cast<uint16*>(Bytes.GetData());
            for (int64 i = 0; i < NumSamples; ++i)
            {
                Out[i] = 0x8000U;
            }
            break;
        }

        case ERuntimeRAWAudioFormat::UInt32:
        {
            uint32* Out = reinterpret_cast<uint32*>(Bytes.GetData());
            for (int64 i = 0; i < NumSamples; ++i)
            {
                Out[i] = 0x80000000U;
            }
            break;
        }
    }

    return Bytes;
}
