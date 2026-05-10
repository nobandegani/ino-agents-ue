// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Qwen3ASR/InoQwen3ASRConstants.h"
#include "Qwen3ASR/InoQwen3ASRRunner.h"
#include "InoQwen3ASRLiteRT.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
    /**
     * Minimal WAV loader for the smoke test. Accepts 16 kHz mono PCM:
     *   - int16 little-endian (most common WAV format)  → converted to float32
     *   - float32 little-endian                          → passed through
     *
     * Anything else is rejected with a clear error message — Phase 4 will add
     * resampling + stereo-downmix via UInoAudioFunctionLibrary; for now the
     * caller pre-converts.
     */
    static bool LoadWav16kMono(const FString& WavPath, TArray<float>& OutSamples)
    {
        OutSamples.Reset();

        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *WavPath))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("WAV: failed to read '%s'."), *WavPath);
            return false;
        }
        if (Bytes.Num() < 44)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("WAV: file too small to be a WAV (%d bytes)."), Bytes.Num());
            return false;
        }
        const uint8* P = Bytes.GetData();
        if (FMemory::Memcmp(P, "RIFF", 4) != 0 ||
            FMemory::Memcmp(P + 8, "WAVE", 4) != 0)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("WAV: missing RIFF/WAVE header."));
            return false;
        }

        // Walk chunks until we find "fmt " and "data".
        uint16 FormatCode = 0, NumChannels = 0, BitsPerSample = 0;
        uint32 SampleRate = 0;
        const uint8* DataPtr = nullptr;
        uint32 DataSize = 0;

        int32 Cursor = 12;  // start after "RIFF" + size + "WAVE"
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
            // Chunks are padded to even length per the spec.
            const uint32 Stride = 8 + ChunkSize + (ChunkSize & 1u);
            Cursor += Stride;
        }

        if (!DataPtr)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("WAV: no 'data' chunk found."));
            return false;
        }
        if (NumChannels != 1 || SampleRate != (uint32)InoQwen3ASR::kSampleRate)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("WAV: need 16 kHz mono — got %d ch @ %u Hz. "
                     "Pre-convert with ffmpeg: -ar 16000 -ac 1"),
                NumChannels, SampleRate);
            return false;
        }
        if (FormatCode == 1 && BitsPerSample == 16)
        {
            // PCM int16 little-endian.
            const int32 NumSamples = DataSize / 2;
            OutSamples.SetNumUninitialized(NumSamples);
            const int16* P16 = reinterpret_cast<const int16*>(DataPtr);
            constexpr float Scale = 1.0f / 32768.0f;
            for (int32 i = 0; i < NumSamples; ++i)
            {
                OutSamples[i] = static_cast<float>(P16[i]) * Scale;
            }
        }
        else if (FormatCode == 3 && BitsPerSample == 32)
        {
            // IEEE float32 PCM.
            const int32 NumSamples = DataSize / 4;
            OutSamples.SetNumUninitialized(NumSamples);
            FMemory::Memcpy(OutSamples.GetData(), DataPtr, NumSamples * sizeof(float));
        }
        else
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("WAV: unsupported format (code=%u, bits=%u). Need int16 (code=1, bits=16) "
                     "or float32 (code=3, bits=32)."),
                FormatCode, BitsPerSample);
            return false;
        }
        return true;
    }

    static FString ResolveModelPath()
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (!Plugin.IsValid()) { return FString(); }
        return FPaths::Combine(Plugin->GetBaseDir(),
            TEXT("Qwen3ASR"), TEXT("LiteRT"), TEXT("models"),
            TEXT("qwen3_asr_0.6b_5s_i8.tflite"));
    }

    static FString ResolveVocabPath()
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (!Plugin.IsValid()) { return FString(); }
        return FPaths::Combine(Plugin->GetBaseDir(),
            TEXT("Qwen3ASR"), TEXT("LiteRT"), TEXT("tokenizer"),
            TEXT("vocab.json"));
    }

    /**
     * Full audio→text smoke test. Usage:
     *   Ino.Qwen3ASRLiteRT.TranscribeTest <absolute_or_relative_wav_path>
     *
     * If no path given, defaults to Plugins/InoAgents/Qwen3ASR/LiteRT/samples/test.wav.
     *
     * Logs:
     *   - per-stage timings (mel / encode / decode)
     *   - the raw token IDs produced
     *   - the final detokenized text
     *
     * If the text looks like garbage but timings + token IDs look plausible,
     * the most likely issue is a missing chat-template prefix in the decoder's
     * input_ids — adjust the runner's start sequence and re-run.
     */
    void RunTranscribeTest(const TArray<FString>& Args)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("=== Ino.Qwen3ASRLiteRT.TranscribeTest ==="));

        FString WavPath;
        if (Args.Num() > 0)
        {
            WavPath = Args[0];
        }
        else
        {
            const TSharedPtr<IPlugin> Plugin =
                IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
            if (Plugin.IsValid())
            {
                WavPath = FPaths::Combine(Plugin->GetBaseDir(),
                    TEXT("Qwen3ASR"), TEXT("LiteRT"), TEXT("samples"), TEXT("test.wav"));
            }
        }

        if (WavPath.IsEmpty() || !FPaths::FileExists(WavPath))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("TranscribeTest: WAV missing — %s"), *WavPath);
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("Provide a 16 kHz mono PCM WAV at that path, or pass one as arg."));
            return;
        }

        // ---- Step 1: load WAV ----
        TArray<float> Audio;
        const double T0Wav = FPlatformTime::Seconds();
        if (!LoadWav16kMono(WavPath, Audio))
        {
            return;
        }
        const double WavSec = FPlatformTime::Seconds() - T0Wav;
        const float DurationSec = Audio.Num() / static_cast<float>(InoQwen3ASR::kSampleRate);
        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("TranscribeTest: loaded WAV — %d samples (%.2fs of audio) in %.3fs"),
            Audio.Num(), DurationSec, WavSec);
        if (Audio.Num() > InoQwen3ASR::kAudioWindowSamples)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Warning,
                TEXT("TranscribeTest: audio is longer than %d samples — only the first 5s will be used."),
                InoQwen3ASR::kAudioWindowSamples);
        }

        // ---- Step 2: load model + tokenizer ----
        const FString ModelPath = ResolveModelPath();
        const FString VocabPath = ResolveVocabPath();
        if (!FPaths::FileExists(ModelPath))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("TranscribeTest: model missing — %s"), *ModelPath);
            return;
        }
        if (!FPaths::FileExists(VocabPath))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("TranscribeTest: vocab.json missing — %s"), *VocabPath);
            return;
        }

        FInoQwen3ASRRunner Runner;
        const double T0Load = FPlatformTime::Seconds();
        if (!Runner.LoadModel(ModelPath))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error, TEXT("TranscribeTest: model load failed."));
            return;
        }
        if (!Runner.LoadTokenizer(VocabPath))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error, TEXT("TranscribeTest: tokenizer load failed."));
            return;
        }
        const double LoadSec = FPlatformTime::Seconds() - T0Load;
        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("TranscribeTest: model + tokenizer loaded in %.2fs"), LoadSec);

        // ---- Step 3: transcribe ----
        FString Text;
        TArray<int32> TokenIds;
        FInoQwen3ASRTranscribeStats Stats;
        if (!Runner.Transcribe(Audio, Text, &TokenIds, &Stats))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error, TEXT("TranscribeTest: Transcribe() failed."));
            return;
        }

        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("TranscribeTest: timings — mel=%.3fs encode=%.3fs decode=%.3fs total=%.3fs (%d tokens)"),
            Stats.MelSeconds, Stats.EncodeSeconds, Stats.DecodeSeconds, Stats.TotalSeconds,
            Stats.NumGeneratedTokens);

        // Print the token IDs in groups for readability.
        FString IdsLine;
        for (int32 i = 0; i < TokenIds.Num(); ++i)
        {
            IdsLine += FString::Printf(TEXT("%s%d"), i == 0 ? TEXT("") : TEXT(", "), TokenIds[i]);
        }
        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("TranscribeTest: token IDs = [%s]"), *IdsLine);

        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("TranscribeTest: text (%d chars) ↓"), Text.Len());
        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("                 ===================="));
        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("                 %s"), *Text);
        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("                 ===================="));
    }

    static FAutoConsoleCommand GTranscribeTestCmd(
        TEXT("Ino.Qwen3ASRLiteRT.TranscribeTest"),
        TEXT("End-to-end Qwen3-ASR smoke test. Usage: Ino.Qwen3ASRLiteRT.TranscribeTest "
             "[wav_path]. Requires the model + vocab.json to be installed at "
             "Plugins/InoAgents/Qwen3ASR/LiteRT/. WAV must be 16 kHz mono PCM "
             "(int16 or float32)."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunTranscribeTest));
}
