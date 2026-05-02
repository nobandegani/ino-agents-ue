// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoQwen3ASRMel.h"

#include "InoQwen3ASRConstants.h"
#include "InoQwen3ASRLiteRT.h"

namespace
{
    // Number of unique frequency bins in a length-kNFft real-input DFT
    // (kNFft/2 + 1 = 201 for kNFft=400). The other bins are conjugates
    // and don't add information.
    constexpr int32 kNumFreqBins = InoQwen3ASR::kNFft / 2 + 1;

    // Whisper post-log normalization constants — match audio.py:
    //     log_spec = max(log_spec, log_spec.max() - 8.0)
    //     log_spec = (log_spec + 4.0) / 4.0
    constexpr float kLogClampDelta = 8.0f;
    constexpr float kLogNormBias   = 4.0f;
    constexpr float kLogNormScale  = 4.0f;

    // Slaney mel-scale parameters from librosa.filters.mel:
    //   below 1000 Hz: linear in (f / (200/3))
    //   above 1000 Hz: 15 + log(f / 1000) / (log(6.4) / 27)
    constexpr float kSlaneyFsp        = 200.0f / 3.0f;
    constexpr float kSlaneyMinLogHz   = 1000.0f;
    constexpr float kSlaneyMinLogMel  = kSlaneyMinLogHz / kSlaneyFsp;          // = 15.0
    static const float kSlaneyLogStep = FMath::Loge(6.4f) / 27.0f;             // ~0.06875

    /** Convert a frequency in Hz to Slaney-style mel. */
    static float HzToMelSlaney(float Hz)
    {
        if (Hz < kSlaneyMinLogHz)
        {
            return Hz / kSlaneyFsp;
        }
        return kSlaneyMinLogMel + FMath::Loge(Hz / kSlaneyMinLogHz) / kSlaneyLogStep;
    }

    /** Convert a Slaney mel value back to Hz. */
    static float MelToHzSlaney(float Mel)
    {
        if (Mel < kSlaneyMinLogMel)
        {
            return Mel * kSlaneyFsp;
        }
        return kSlaneyMinLogHz * FMath::Exp(kSlaneyLogStep * (Mel - kSlaneyMinLogMel));
    }
}

FInoQwen3ASRMel::FInoQwen3ASRMel()
{
    BuildHannWindow();
    BuildMelFilterbank();
}

void FInoQwen3ASRMel::BuildHannWindow()
{
    // Periodic Hann (matches torch.hann_window with periodic=True default):
    //     w[n] = 0.5 - 0.5 * cos(2*pi*n / N)     for n in 0..N-1
    // Using the *non-symmetric* divisor (N rather than N-1) is what makes it
    // periodic; this is what PyTorch / librosa STFT use by default.
    HannWindow.SetNumUninitialized(InoQwen3ASR::kNFft);
    const float TwoPiOverN = 2.0f * PI / static_cast<float>(InoQwen3ASR::kNFft);
    for (int32 n = 0; n < InoQwen3ASR::kNFft; ++n)
    {
        HannWindow[n] = 0.5f - 0.5f * FMath::Cos(TwoPiOverN * n);
    }
}

void FInoQwen3ASRMel::BuildMelFilterbank()
{
    // Slaney mel filterbank with Slaney normalization, matching:
    //     librosa.filters.mel(sr=16000, n_fft=400, n_mels=128,
    //                         fmin=0.0, fmax=8000.0, htk=False, norm='slaney')
    //
    // Whisper loads its precomputed filterbank from mel_filters.npz; the
    // contents are exactly the output of the call above, so building it
    // ourselves here is bit-identical.

    MelFilterbank.SetNumZeroed(InoQwen3ASR::kNMels * kNumFreqBins);

    // FFT bin frequencies in Hz: linspace(0, sr/2, n_freq_bins).
    TArray<float, TInlineAllocator<256>> FftFreqs;
    FftFreqs.SetNumUninitialized(kNumFreqBins);
    const float HzPerBin = static_cast<float>(InoQwen3ASR::kSampleRate) /
                           static_cast<float>(InoQwen3ASR::kNFft);
    for (int32 i = 0; i < kNumFreqBins; ++i)
    {
        FftFreqs[i] = HzPerBin * i;
    }

    // Mel-spaced anchor frequencies: linspace in mel from fmin to fmax,
    // then convert each back to Hz. We need n_mels + 2 anchors so that
    // each filter has a left, center, and right boundary.
    const int32 NumAnchors = InoQwen3ASR::kNMels + 2;
    TArray<float, TInlineAllocator<256>> HzAnchors;
    HzAnchors.SetNumUninitialized(NumAnchors);
    {
        const float MinMel = HzToMelSlaney(InoQwen3ASR::kMelFmin);
        const float MaxMel = HzToMelSlaney(InoQwen3ASR::kMelFmax);
        const float MelStep = (MaxMel - MinMel) / static_cast<float>(NumAnchors - 1);
        for (int32 i = 0; i < NumAnchors; ++i)
        {
            HzAnchors[i] = MelToHzSlaney(MinMel + MelStep * i);
        }
    }

    // Triangular filters + Slaney normalization (per-filter scale by
    // 2 / (right_anchor - left_anchor)).
    for (int32 m = 0; m < InoQwen3ASR::kNMels; ++m)
    {
        const float LeftHz   = HzAnchors[m];
        const float CenterHz = HzAnchors[m + 1];
        const float RightHz  = HzAnchors[m + 2];

        const float LeftSlope  = 1.0f / (CenterHz - LeftHz);
        const float RightSlope = 1.0f / (RightHz - CenterHz);
        const float SlaneyNorm = 2.0f / (RightHz - LeftHz);

        float* RowOut = &MelFilterbank[m * kNumFreqBins];
        for (int32 k = 0; k < kNumFreqBins; ++k)
        {
            const float Hz = FftFreqs[k];
            float Weight;
            if (Hz <= LeftHz || Hz >= RightHz)
            {
                Weight = 0.0f;
            }
            else if (Hz <= CenterHz)
            {
                Weight = (Hz - LeftHz) * LeftSlope;
            }
            else
            {
                Weight = (RightHz - Hz) * RightSlope;
            }
            RowOut[k] = Weight * SlaneyNorm;
        }
    }
}

void FInoQwen3ASRMel::Compute(TArrayView<const float> Audio, TArray<float>& OutLogMel) const
{
    using namespace InoQwen3ASR;

    // Step 1: build a kNFft/2-reflection-padded copy of the first kAudioWindowSamples
    // of the input. Trailing samples beyond kAudioWindowSamples are ignored;
    // shorter inputs get zero-padded.
    constexpr int32 PadHalf = kNFft / 2;                         // 200
    constexpr int32 PaddedLen = kAudioWindowSamples + 2 * PadHalf; // 80400

    TArray<float> Padded;
    Padded.SetNumZeroed(PaddedLen);
    {
        const int32 NumCopy = FMath::Min(Audio.Num(), kAudioWindowSamples);
        // Center: Padded[PadHalf .. PadHalf+NumCopy)
        FMemory::Memcpy(Padded.GetData() + PadHalf, Audio.GetData(), NumCopy * sizeof(float));
        // Trailing samples after NumCopy stay zero (treat shorter input as silence-padded).

        // Left reflection: Padded[0..PadHalf) = audio[PadHalf .. 1] (reverse,
        // skipping audio[0]).
        const float* Center = Padded.GetData() + PadHalf;
        for (int32 i = 0; i < PadHalf; ++i)
        {
            // numpy reflect: Padded[PadHalf - 1 - i] = Center[i + 1]
            Padded[PadHalf - 1 - i] = Center[i + 1];
        }
        // Right reflection: Padded[PadHalf+kAudioWindowSamples..] mirrors the
        // last samples (numpy reflect skips the boundary sample).
        for (int32 i = 0; i < PadHalf; ++i)
        {
            Padded[PadHalf + kAudioWindowSamples + i] =
                Center[kAudioWindowSamples - 2 - i];
        }
    }

    // Step 2: STFT — for each of kMelFrames+1 frames produce a length-kNumFreqBins
    // power spectrum |X|^2. We compute kMelFrames+1 frames then drop the last
    // (matching torch.stft + Whisper's [..., :-1] slice).
    const int32 NumStftFrames = kMelFrames + 1;  // 501

    // Frame magnitude-squared spectra: row-major (NumStftFrames, kNumFreqBins).
    TArray<float> PowerSpec;
    PowerSpec.SetNumUninitialized(NumStftFrames * kNumFreqBins);

    TArray<float, TInlineAllocator<512>> FrameBuf;
    FrameBuf.SetNumUninitialized(kNFft);

    for (int32 f = 0; f < NumStftFrames; ++f)
    {
        const int32 Start = f * kHopLength;

        // Apply Hann window in place into FrameBuf.
        for (int32 n = 0; n < kNFft; ++n)
        {
            FrameBuf[n] = Padded[Start + n] * HannWindow[n];
        }

        // Naive DFT: for each output bin k in 0..kNumFreqBins-1,
        //   X[k] = sum_n frame[n] * exp(-2*pi*i * k * n / kNFft)
        // |X[k]|^2 = re^2 + im^2. ~322k mul-adds per frame.
        float* PowRow = &PowerSpec[f * kNumFreqBins];
        const float TwoPiOverN = 2.0f * PI / static_cast<float>(kNFft);
        for (int32 k = 0; k < kNumFreqBins; ++k)
        {
            float Re = 0.0f, Im = 0.0f;
            const float WK = TwoPiOverN * k;
            for (int32 n = 0; n < kNFft; ++n)
            {
                const float Phase = WK * n;
                Re += FrameBuf[n] * FMath::Cos(Phase);
                Im -= FrameBuf[n] * FMath::Sin(Phase);
            }
            PowRow[k] = Re * Re + Im * Im;
        }
    }

    // Step 3: project through mel filterbank.
    // mel_spec[mel, frame] = sum_k filterbank[mel, k] * power_spec[frame, k]
    //
    // Output layout matches Whisper: row-major (kNMels, kMelFrames).
    OutLogMel.SetNumUninitialized(kNMels * kMelFrames);
    for (int32 m = 0; m < kNMels; ++m)
    {
        const float* FilterRow = &MelFilterbank[m * kNumFreqBins];
        for (int32 f = 0; f < kMelFrames; ++f)
        {
            const float* PowRow = &PowerSpec[f * kNumFreqBins];
            float Sum = 0.0f;
            for (int32 k = 0; k < kNumFreqBins; ++k)
            {
                Sum += FilterRow[k] * PowRow[k];
            }
            OutLogMel[m * kMelFrames + f] = Sum;
        }
    }

    // Step 4: log10 with floor 1e-10.
    constexpr float MinPower = 1.0e-10f;
    constexpr float Log10E = 0.4342944819032518f;  // 1 / ln(10)
    float MaxLog = -FLT_MAX;
    for (int32 i = 0; i < OutLogMel.Num(); ++i)
    {
        const float Clamped = FMath::Max(OutLogMel[i], MinPower);
        const float Lg = FMath::Loge(Clamped) * Log10E;
        OutLogMel[i] = Lg;
        if (Lg > MaxLog) { MaxLog = Lg; }
    }

    // Step 5: Whisper normalization — clamp to [max-8, max] then (x+4)/4.
    const float Floor = MaxLog - kLogClampDelta;
    float MinOut = FLT_MAX, MaxOut = -FLT_MAX;
    for (int32 i = 0; i < OutLogMel.Num(); ++i)
    {
        float V = FMath::Max(OutLogMel[i], Floor);
        V = (V + kLogNormBias) / kLogNormScale;
        OutLogMel[i] = V;
        if (V < MinOut) { MinOut = V; }
        if (V > MaxOut) { MaxOut = V; }
    }
    LastMin = MinOut;
    LastMax = MaxOut;

    UE_LOG(LogInoQwen3ASRLiteRT, Verbose,
        TEXT("Mel: input=%d samples, output=(%d, %d), value range [%.4f, %.4f]"),
        Audio.Num(), kNMels, kMelFrames, MinOut, MaxOut);
}
