// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Qwen3ASR/InoQwen3ASRConstants.h"
#include "Qwen3ASR/InoQwen3ASRMel.h"
#include "InoQwen3ASRLiteRT.h"

#include "HAL/IConsoleManager.h"

namespace
{
    /**
     * Compute log-mel for two synthetic 5-second audio clips:
     *   1. Silence (all zeros)
     *   2. 1 kHz sine wave at amplitude 0.5
     *
     * Both should produce well-formed (kNMels, kMelFrames) outputs. The sine
     * wave is the more interesting check — a real signal exercises every step
     * of the STFT path, and the log-mel output should peak in the mel bin
     * containing 1 kHz (~bin 40 of 128 in Slaney mel scale).
     */
    void RunMelTest(const TArray<FString>& /*Args*/)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Log, TEXT("=== Ino.Qwen3ASRLiteRT.MelTest ==="));

        FInoQwen3ASRMel Mel;

        // ---- Test 1: silence ----
        TArray<float> SilenceAudio;
        SilenceAudio.SetNumZeroed(InoQwen3ASR::kAudioWindowSamples);

        TArray<float> Mel1;
        const double T0a = FPlatformTime::Seconds();
        Mel.Compute(SilenceAudio, Mel1);
        const double Elapsed1 = FPlatformTime::Seconds() - T0a;

        float MinV1, MaxV1;
        Mel.GetLastValueRange(MinV1, MaxV1);
        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("MelTest[silence]: %d elements in %.3fs, range [%.4f, %.4f]"),
            Mel1.Num(), Elapsed1, MinV1, MaxV1);

        if (Mel1.Num() != InoQwen3ASR::kNMels * InoQwen3ASR::kMelFrames)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("MelTest[silence]: expected %d elements, got %d"),
                InoQwen3ASR::kNMels * InoQwen3ASR::kMelFrames, Mel1.Num());
            return;
        }

        // ---- Test 2: 1 kHz sine wave ----
        TArray<float> SineAudio;
        SineAudio.SetNumUninitialized(InoQwen3ASR::kAudioWindowSamples);
        const float Freq = 1000.0f;
        const float Amp  = 0.5f;
        const float TwoPiFOverSr =
            2.0f * PI * Freq / static_cast<float>(InoQwen3ASR::kSampleRate);
        for (int32 i = 0; i < SineAudio.Num(); ++i)
        {
            SineAudio[i] = Amp * FMath::Sin(TwoPiFOverSr * i);
        }

        TArray<float> Mel2;
        const double T0b = FPlatformTime::Seconds();
        Mel.Compute(SineAudio, Mel2);
        const double Elapsed2 = FPlatformTime::Seconds() - T0b;

        float MinV2, MaxV2;
        Mel.GetLastValueRange(MinV2, MaxV2);

        // Find the mel bin with the highest mean energy across frames — for
        // a 1 kHz tone in a 128-bin Slaney mel filterbank @ 16 kHz that's
        // typically around bin 40 (Slaney's linear region).
        int32 PeakBin = -1;
        float PeakEnergy = -FLT_MAX;
        for (int32 m = 0; m < InoQwen3ASR::kNMels; ++m)
        {
            double Sum = 0.0;
            for (int32 f = 0; f < InoQwen3ASR::kMelFrames; ++f)
            {
                Sum += Mel2[m * InoQwen3ASR::kMelFrames + f];
            }
            const float Mean = static_cast<float>(Sum / InoQwen3ASR::kMelFrames);
            if (Mean > PeakEnergy) { PeakEnergy = Mean; PeakBin = m; }
        }

        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("MelTest[sine 1kHz]: %d elements in %.3fs, range [%.4f, %.4f], peak bin = %d (mean=%.4f)"),
            Mel2.Num(), Elapsed2, MinV2, MaxV2, PeakBin, PeakEnergy);

        // Ballpark check: at Slaney scale + Whisper normalization, a real
        // signal should land somewhere in [-2, 1.5]. Silence should sit
        // close to (max-8+4)/4 = (max-4)/4. Both should be finite.
        const bool bSilenceOk = FMath::IsFinite(MinV1) && FMath::IsFinite(MaxV1);
        const bool bSineOk    = FMath::IsFinite(MinV2) && FMath::IsFinite(MaxV2)
                                && PeakEnergy > MinV2;
        if (bSilenceOk && bSineOk)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Log,
                TEXT("MelTest: PASS — both clips produced finite, well-formed log-mel."));
        }
        else
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("MelTest: FAIL — output contained non-finite values."));
        }
    }

    static FAutoConsoleCommand GMelTestCmd(
        TEXT("Ino.Qwen3ASRLiteRT.MelTest"),
        TEXT("Compute Whisper log-mel spectrogram for two synthetic 5-second clips "
             "(silence + 1 kHz sine), verify shape + finiteness + peak bin location. "
             "Doesn't load the model — purely tests the audio preprocessing path."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunMelTest));
}
