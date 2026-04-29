// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Audio/InoAudioFunctionLibrary.h"

#include "HAL/PlatformMemory.h"
#include "Math/RandomStream.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/FileHelper.h"

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

bool UInoAudioFunctionLibrary::SaveInt16PcmAsWav(
    const FString& FilePath,
    const TArray<uint8>& PcmBytes,
    int32 SampleRate)
{
    if (FilePath.IsEmpty())
    {
        UE_LOG(LogTemp, Warning,
               TEXT("SaveInt16PcmAsWav: empty file path"));
        return false;
    }

    // Same impl as the TArrayView entry point — surface the result
    // as a Blueprint-callable bool.
    return WriteInt16PcmBytesAsWav(FilePath, MakeArrayView(PcmBytes), SampleRate);
}

// ---------------------------------------------------------------------
// C++-only static helpers (promoted from InoChatterboxAudioIO).
// ---------------------------------------------------------------------

bool UInoAudioFunctionLibrary::ReadMonoWavAsFloat32(
    const FString& Path,
    TArray<float>& OutSamples,
    int32& OutSampleRate,
    FString* OutError)
{
    OutSamples.Reset();
    OutSampleRate = 0;

    auto Fail = [&](const FString& Msg) -> bool
    {
        if (OutError) { *OutError = Msg; }
        return false;
    };

    TArray<uint8> Buf;
    if (!FFileHelper::LoadFileToArray(Buf, *Path))
    {
        return Fail(FString::Printf(TEXT("cannot read %s"), *Path));
    }
    if (Buf.Num() < 44)
    {
        return Fail(TEXT("file too small to be a WAV"));
    }

    const uint8* D = Buf.GetData();
    if (FMemory::Memcmp(D + 0, "RIFF", 4) != 0)
    {
        return Fail(TEXT("missing 'RIFF' tag"));
    }
    if (FMemory::Memcmp(D + 8, "WAVE", 4) != 0)
    {
        return Fail(TEXT("missing 'WAVE' tag"));
    }
    if (FMemory::Memcmp(D + 12, "fmt ", 4) != 0)
    {
        return Fail(TEXT("missing 'fmt ' chunk at expected offset"));
    }

    // fmt chunk (canonical layout — may have a longer tail for
    // AudioFormat != 1, but we only read the first 16 bytes'
    // worth of header fields which are the same across variants).
    const uint32 FmtSize       = *reinterpret_cast<const uint32*>(D + 16);
    const uint16 AudioFormat   = *reinterpret_cast<const uint16*>(D + 20);
    const uint16 NumChannels   = *reinterpret_cast<const uint16*>(D + 22);
    const uint32 SampleRate    = *reinterpret_cast<const uint32*>(D + 24);
    const uint16 BitsPerSample = *reinterpret_cast<const uint16*>(D + 34);

    if (NumChannels != 1)
    {
        return Fail(FString::Printf(
            TEXT("NumChannels=%u (need mono=1)"), (uint32)NumChannels));
    }

    // Decide on the per-sample decode path based on (AudioFormat,
    // BitsPerSample). We bail on anything else explicitly rather
    // than trying to guess.
    enum class EDecode { Int16PCM, Float32IEEE };
    EDecode Decode;
    int32 BytesPerSample;
    if (AudioFormat == 1 && BitsPerSample == 16)
    {
        Decode = EDecode::Int16PCM;
        BytesPerSample = 2;
    }
    else if (AudioFormat == 3 && BitsPerSample == 32)
    {
        Decode = EDecode::Float32IEEE;
        BytesPerSample = 4;
    }
    else
    {
        return Fail(FString::Printf(
            TEXT("unsupported format AudioFormat=%u BitsPerSample=%u ")
            TEXT("(handled: PCM 16-bit and IEEE float 32-bit only)"),
            (uint32)AudioFormat, (uint32)BitsPerSample));
    }

    // Locate the 'data' chunk. Some encoders insert 'LIST' / 'JUNK'
    // / 'fact' chunks between 'fmt ' and 'data', so scan past fmt's
    // end rather than assuming offset 36. AudioFormat=3 specifically
    // tends to come with a 'fact' chunk.
    int64 ScanOffset = 12 + 8 + (int64)FmtSize;
    int64 DataStart  = -1;
    int64 DataBytes  = 0;
    while (ScanOffset + 8 <= Buf.Num())
    {
        if (FMemory::Memcmp(D + ScanOffset, "data", 4) == 0)
        {
            DataBytes = (int64)(*reinterpret_cast<const uint32*>(D + ScanOffset + 4));
            DataStart = ScanOffset + 8;
            break;
        }
        const uint32 ChunkSize = *reinterpret_cast<const uint32*>(D + ScanOffset + 4);
        // Chunk sizes are word-aligned (add pad byte if odd). See the
        // WAVE spec — chunk bodies that are odd length are followed by
        // one padding byte to keep the next header 16-bit aligned.
        const int64 Padded = (int64)ChunkSize + (ChunkSize & 1);
        ScanOffset += 8 + Padded;
    }
    if (DataStart < 0)
    {
        return Fail(TEXT("no 'data' chunk found"));
    }
    if (DataStart + DataBytes > (int64)Buf.Num())
    {
        return Fail(TEXT("'data' chunk extends past end of file"));
    }

    const int64 NumSamples = DataBytes / (int64)BytesPerSample;
    OutSamples.SetNumUninitialized((int32)NumSamples);

    if (Decode == EDecode::Int16PCM)
    {
        const int16* Src = reinterpret_cast<const int16*>(D + DataStart);
        for (int64 i = 0; i < NumSamples; ++i)
        {
            OutSamples[(int32)i] = (float)Src[i] * (1.0f / 32768.0f);
        }
    }
    else  // Float32IEEE
    {
        // The WAV data is already fp32 in [-1, +1]; a flat memcpy
        // is the right move (byte layout is little-endian on both
        // x86 and ARM64 UE targets).
        const float* Src = reinterpret_cast<const float*>(D + DataStart);
        FMemory::Memcpy(OutSamples.GetData(), Src, (SIZE_T)NumSamples * sizeof(float));
    }

    OutSampleRate = (int32)SampleRate;
    return true;
}

bool UInoAudioFunctionLibrary::WriteInt16PcmBytesAsWav(
    const FString& Path,
    TArrayView<const uint8> PcmBytes,
    int32 SampleRate)
{
    const int32 NumBytes = PcmBytes.Num();
    if (NumBytes <= 0 || (NumBytes & 1) != 0)
    {
        return false;   // empty OR not int16-aligned
    }

    TArray<uint8> Buf;
    Buf.Reserve(44 + NumBytes);

    auto AppendU16 = [&Buf](uint16 V) { Buf.Append((const uint8*)&V, 2); };
    auto AppendU32 = [&Buf](uint32 V) { Buf.Append((const uint8*)&V, 4); };
    auto AppendTag = [&Buf](const char* Tag) { Buf.Append((const uint8*)Tag, 4); };

    const uint32 DataBytes = (uint32)NumBytes;
    const uint32 RiffSize  = 36 + DataBytes;   // total file size - 8

    AppendTag("RIFF");
    AppendU32(RiffSize);
    AppendTag("WAVE");

    AppendTag("fmt ");
    AppendU32(16);                             // fmt chunk size
    AppendU16(1);                              // PCM
    AppendU16(1);                              // mono
    AppendU32((uint32)SampleRate);
    AppendU32((uint32)SampleRate * 2);         // byte rate
    AppendU16(2);                              // block align
    AppendU16(16);                             // bits per sample

    AppendTag("data");
    AppendU32(DataBytes);

    // Copy the PCM body in a single memcpy — no per-sample work.
    const int32 BodyStart = Buf.Num();
    Buf.SetNumUninitialized(BodyStart + NumBytes);
    FMemory::Memcpy(Buf.GetData() + BodyStart, PcmBytes.GetData(), NumBytes);

    return FFileHelper::SaveArrayToFile(Buf, *Path);
}

bool UInoAudioFunctionLibrary::WriteMonoInt16Wav(
    const FString& Path,
    TArrayView<const float> Samples,
    int32 SampleRate)
{
    if (Samples.Num() <= 0)
    {
        return false;
    }

    // Quantize once via the shared helper, then delegate framing to
    // the byte-oriented WAV writer. Two source-of-truth reduction:
    // one quantization path, one WAV-header path.
    TArray<uint8> PcmBytes;
    Float32ToInt16PcmBytesMono(Samples, PcmBytes);
    return WriteInt16PcmBytesAsWav(Path, PcmBytes, SampleRate);
}

bool UInoAudioFunctionLibrary::Int16PcmBytesToFloat32Mono(
    TArrayView<const uint8> PcmBytes,
    TArray<float>& OutSamples,
    FString* OutError)
{
    OutSamples.Reset();

    const int32 NumBytes = PcmBytes.Num();
    if ((NumBytes & 1) != 0)
    {
        if (OutError)
        {
            *OutError = FString::Printf(
                TEXT("Int16PcmBytesToFloat32Mono: byte count %d is not a multiple of 2 ")
                TEXT("(int16 PCM requires 2-byte-aligned data)"), NumBytes);
        }
        return false;
    }
    if (NumBytes == 0)
    {
        return true;   // legit empty buffer; caller will check Num()
    }

    const int32 NumSamples = NumBytes / 2;
    OutSamples.SetNumUninitialized(NumSamples);

    // x86_64 and ARM64 are both little-endian, matching WAV file data
    // layout. A reinterpret_cast over the byte buffer is correct and
    // requires no byte swap.
    const int16* Src = reinterpret_cast<const int16*>(PcmBytes.GetData());
    for (int32 i = 0; i < NumSamples; ++i)
    {
        OutSamples[i] = (float)Src[i] * (1.0f / 32768.0f);
    }
    return true;
}

void UInoAudioFunctionLibrary::Float32ToInt16PcmBytesMono(
    TArrayView<const float> Samples,
    TArray<uint8>& OutBytes)
{
    const int32 NumSamples = Samples.Num();
    OutBytes.SetNumUninitialized(NumSamples * 2);

    if (NumSamples == 0)
    {
        return;
    }

    int16* Dst = reinterpret_cast<int16*>(OutBytes.GetData());
    for (int32 i = 0; i < NumSamples; ++i)
    {
        const float Clamped = FMath::Clamp(Samples[i], -1.0f, 1.0f);
        Dst[i] = (int16)FMath::RoundToInt(Clamped * 32767.0f);
    }
}
