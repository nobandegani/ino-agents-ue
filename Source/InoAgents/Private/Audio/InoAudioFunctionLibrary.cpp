// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoAudioFunctionLibrary.h"

#include "Math/RandomStream.h"

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

    /** Shared size-and-allocate helper. Returns a zero-allocated TArray on
     *  any failure path; callers can then early-return with Bytes. */
    bool ComputeByteLayout(
        float DurationMs, int32 SampleRate, int32 NumChannels,
        ERuntimeRAWAudioFormat Format,
        int64& OutNumFrames, int64& OutNumSamples,
        int64& OutNumBytes, int32& OutBytesPerSample)
    {
        const float ClampedMs   = FMath::Max(DurationMs, 0.0f);
        const int32 ClampedRate = FMath::Clamp(SampleRate, 1, 192000);
        const int32 ClampedCh   = FMath::Clamp(NumChannels, 1, 8);

        OutBytesPerSample = BytesPerSampleForFormat(Format);
        if (OutBytesPerSample <= 0)
        {
            return false;
        }

        OutNumFrames = static_cast<int64>(
            static_cast<double>(ClampedMs) / 1000.0 * static_cast<double>(ClampedRate));
        OutNumSamples = OutNumFrames * ClampedCh;
        OutNumBytes   = OutNumSamples * OutBytesPerSample;
        return OutNumBytes > 0 && OutNumBytes <= MAX_int32;
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

TArray<uint8> UInoAudioFunctionLibrary::GenerateDitheredSilence(
    float DurationMs, int32 SampleRate, int32 NumChannels,
    ERuntimeRAWAudioFormat Format, float NoiseAmplitude)
{
    int64 NumFrames       = 0;
    int64 NumSamples      = 0;
    int64 NumBytes        = 0;
    int32 BytesPerSample  = 0;
    if (!ComputeByteLayout(DurationMs, SampleRate, NumChannels, Format,
                           NumFrames, NumSamples, NumBytes, BytesPerSample))
    {
        return {};
    }

    const float ClampedAmp = FMath::Clamp(NoiseAmplitude, 0.0f, 1.0f);

    // Amplitude 0 is just plain silence — save a copy by delegating.
    if (ClampedAmp <= 0.0f)
    {
        return GenerateEmptyRawAudio(DurationMs, SampleRate, NumChannels, Format);
    }

    TArray<uint8> Bytes;
    Bytes.SetNumUninitialized(static_cast<int32>(NumBytes));

    // Uniform noise centred on 0. Balanced around silence so the long-term
    // DC offset of the buffer is still zero.
    auto RandSigned = []() -> float { return FMath::FRand() * 2.0f - 1.0f; };

    switch (Format)
    {
        case ERuntimeRAWAudioFormat::Int8:
        {
            const int32 Mag = FMath::Max(1, FMath::FloorToInt(ClampedAmp * 127.0f));
            int8* Out = reinterpret_cast<int8*>(Bytes.GetData());
            for (int64 i = 0; i < NumSamples; ++i)
            {
                const int32 V = FMath::RoundToInt(RandSigned() * Mag);
                Out[i] = static_cast<int8>(FMath::Clamp(V, -127, 127));
            }
            break;
        }

        case ERuntimeRAWAudioFormat::UInt8:
        {
            const int32 Mag = FMath::Max(1, FMath::FloorToInt(ClampedAmp * 127.0f));
            uint8* Out = Bytes.GetData();
            for (int64 i = 0; i < NumSamples; ++i)
            {
                const int32 V = 128 + FMath::RoundToInt(RandSigned() * Mag);
                Out[i] = static_cast<uint8>(FMath::Clamp(V, 0, 255));
            }
            break;
        }

        case ERuntimeRAWAudioFormat::Int16:
        {
            const int32 Mag = FMath::Max(1, FMath::FloorToInt(ClampedAmp * 32767.0f));
            int16* Out = reinterpret_cast<int16*>(Bytes.GetData());
            for (int64 i = 0; i < NumSamples; ++i)
            {
                const int32 V = FMath::RoundToInt(RandSigned() * Mag);
                Out[i] = static_cast<int16>(FMath::Clamp(V, -32767, 32767));
            }
            break;
        }

        case ERuntimeRAWAudioFormat::UInt16:
        {
            const int32 Mag = FMath::Max(1, FMath::FloorToInt(ClampedAmp * 32767.0f));
            uint16* Out = reinterpret_cast<uint16*>(Bytes.GetData());
            for (int64 i = 0; i < NumSamples; ++i)
            {
                const int32 V = 32768 + FMath::RoundToInt(RandSigned() * Mag);
                Out[i] = static_cast<uint16>(FMath::Clamp(V, 0, 65535));
            }
            break;
        }

        case ERuntimeRAWAudioFormat::Int32:
        {
            const int64 Mag = FMath::Max<int64>(
                1, static_cast<int64>(static_cast<double>(ClampedAmp) * 2147483647.0));
            int32* Out = reinterpret_cast<int32*>(Bytes.GetData());
            for (int64 i = 0; i < NumSamples; ++i)
            {
                const int64 V =
                    static_cast<int64>(static_cast<double>(RandSigned()) * static_cast<double>(Mag));
                Out[i] = static_cast<int32>(FMath::Clamp<int64>(V, -2147483647LL, 2147483647LL));
            }
            break;
        }

        case ERuntimeRAWAudioFormat::UInt32:
        {
            const int64 Mag = FMath::Max<int64>(
                1, static_cast<int64>(static_cast<double>(ClampedAmp) * 2147483647.0));
            uint32* Out = reinterpret_cast<uint32*>(Bytes.GetData());
            for (int64 i = 0; i < NumSamples; ++i)
            {
                const int64 V = 2147483648LL +
                    static_cast<int64>(static_cast<double>(RandSigned()) * static_cast<double>(Mag));
                Out[i] = static_cast<uint32>(FMath::Clamp<int64>(V, 0, 4294967295LL));
            }
            break;
        }

        case ERuntimeRAWAudioFormat::Float32:
        {
            float* Out = reinterpret_cast<float*>(Bytes.GetData());
            for (int64 i = 0; i < NumSamples; ++i)
            {
                Out[i] = RandSigned() * ClampedAmp;
            }
            break;
        }
    }

    return Bytes;
}
