// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxAudioIO.h"

#include "HAL/PlatformMemory.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/FileHelper.h"

namespace InoChatterbox
{

bool ReadMonoWavAsFloat32(
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

bool WriteMonoInt16Wav(
    const FString& Path,
    TArrayView<const float> Samples,
    int32 SampleRate)
{
    const int32 N = Samples.Num();
    if (N <= 0)
    {
        return false;
    }

    TArray<uint8> Buf;
    Buf.Reserve(44 + N * 2);

    auto AppendU16 = [&Buf](uint16 V) { Buf.Append((const uint8*)&V, 2); };
    auto AppendU32 = [&Buf](uint32 V) { Buf.Append((const uint8*)&V, 4); };
    auto AppendTag = [&Buf](const char* Tag) { Buf.Append((const uint8*)Tag, 4); };

    const uint32 DataBytes = (uint32)N * 2;
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

    const int32 SampleStart = Buf.Num();
    Buf.SetNumUninitialized(SampleStart + (int32)DataBytes);
    int16* Out = reinterpret_cast<int16*>(Buf.GetData() + SampleStart);
    for (int32 i = 0; i < N; ++i)
    {
        const float Clamped = FMath::Clamp(Samples[i], -1.0f, 1.0f);
        Out[i] = (int16)FMath::RoundToInt(Clamped * 32767.0f);
    }

    return FFileHelper::SaveArrayToFile(Buf, *Path);
}

} // namespace InoChatterbox
