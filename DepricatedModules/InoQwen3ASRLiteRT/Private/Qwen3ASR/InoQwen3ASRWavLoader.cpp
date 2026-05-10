// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoQwen3ASRWavLoader.h"

#include "InoQwen3ASRConstants.h"
#include "InoQwen3ASRLiteRT.h"

#include "Misc/FileHelper.h"

namespace InoQwen3ASR
{
    bool LoadWav16kMonoFromBytes(TArrayView<const uint8> Bytes, TArray<float>& OutSamples)
    {
        OutSamples.Reset();

        if (Bytes.Num() < 44)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("WAV: payload too small to be a WAV (%d bytes)."), Bytes.Num());
            return false;
        }
        const uint8* P = Bytes.GetData();
        if (FMemory::Memcmp(P, "RIFF", 4) != 0 ||
            FMemory::Memcmp(P + 8, "WAVE", 4) != 0)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error, TEXT("WAV: missing RIFF/WAVE header."));
            return false;
        }

        // Walk chunks until we find both "fmt " and "data". The format chunk
        // can appear before "data"; "fact" / "LIST" / etc. chunks may be
        // interleaved and are skipped silently.
        uint16 FormatCode = 0, NumChannels = 0, BitsPerSample = 0;
        uint32 SampleRate = 0;
        const uint8* DataPtr = nullptr;
        uint32 DataSize = 0;

        int32 Cursor = 12;
        while (Cursor + 8 <= Bytes.Num())
        {
            const uint8* ChunkId   = P + Cursor;
            const uint32 ChunkSize = *reinterpret_cast<const uint32*>(P + Cursor + 4);
            const uint8* ChunkData = P + Cursor + 8;
            if (FMemory::Memcmp(ChunkId, "fmt ", 4) == 0 && ChunkSize >= 16)
            {
                FormatCode    = *reinterpret_cast<const uint16*>(ChunkData + 0);
                NumChannels   = *reinterpret_cast<const uint16*>(ChunkData + 2);
                SampleRate    = *reinterpret_cast<const uint32*>(ChunkData + 4);
                BitsPerSample = *reinterpret_cast<const uint16*>(ChunkData + 14);
            }
            else if (FMemory::Memcmp(ChunkId, "data", 4) == 0)
            {
                DataPtr  = ChunkData;
                DataSize = ChunkSize;
                break;
            }
            // RIFF spec: chunks are padded to even length.
            const uint32 Stride = 8 + ChunkSize + (ChunkSize & 1u);
            Cursor += Stride;
        }

        if (!DataPtr)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error, TEXT("WAV: no 'data' chunk found."));
            return false;
        }
        if (NumChannels != 1 || SampleRate != static_cast<uint32>(InoQwen3ASR::kSampleRate))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("WAV: need %d Hz mono — got %d ch @ %u Hz. Pre-convert with: "
                     "ffmpeg -i in.ext -ar %d -ac 1 -sample_fmt s16 out.wav"),
                InoQwen3ASR::kSampleRate, NumChannels, SampleRate, InoQwen3ASR::kSampleRate);
            return false;
        }
        if (FormatCode == 1 && BitsPerSample == 16)
        {
            const int32 NumSamples = DataSize / 2;
            OutSamples.SetNumUninitialized(NumSamples);
            const int16* P16 = reinterpret_cast<const int16*>(DataPtr);
            constexpr float Scale = 1.0f / 32768.0f;
            for (int32 i = 0; i < NumSamples; ++i)
            {
                OutSamples[i] = static_cast<float>(P16[i]) * Scale;
            }
            return true;
        }
        if (FormatCode == 3 && BitsPerSample == 32)
        {
            const int32 NumSamples = DataSize / 4;
            OutSamples.SetNumUninitialized(NumSamples);
            FMemory::Memcpy(OutSamples.GetData(), DataPtr, NumSamples * sizeof(float));
            return true;
        }
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("WAV: unsupported format (code=%u, bits=%u). Need int16 (1, 16) or float32 (3, 32)."),
            FormatCode, BitsPerSample);
        return false;
    }

    bool LoadWav16kMonoFromDisk(const FString& WavPath, TArray<float>& OutSamples)
    {
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *WavPath))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("WAV: failed to read '%s'."), *WavPath);
            return false;
        }
        return LoadWav16kMonoFromBytes(Bytes, OutSamples);
    }
}
