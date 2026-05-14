// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTTSSynthesisWorker.h"

#include "InoNeuTTSCommon.h"
#include "InoNeuTTSDecoderSession.h"
#include "InoNeuTTSEngineBackend.h"
#include "InoNeuTTSPromptBuilder.h"
#include "InoNeuTTSRunner.h"

#include "InoAgentsLog.h"
#include "InoSpeakNGBPLibrary.h"

#include "HAL/PlatformTime.h"
#include "Math/UnrealMathUtility.h"

namespace InoNeuTTSNative
{

namespace
{
    static bool IsCancelled(const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe>& Flag)
    {
        return Flag.IsValid() && Flag->load(std::memory_order_acquire);
    }

    /** Convert float32 samples in [-1, 1] to int16 PCM LE bytes (mono).
     *  Round-half-away-from-zero, clamp to int16 range. */
    static TArray<uint8> Float32ToInt16PcmBytesMono(TArrayView<const float> Samples)
    {
        TArray<uint8> Out;
        Out.SetNumUninitialized(Samples.Num() * sizeof(int16));
        int16* Dst = reinterpret_cast<int16*>(Out.GetData());
        for (int32 i = 0; i < Samples.Num(); ++i)
        {
            const float Clamped = FMath::Clamp(Samples[i], -1.0f, 1.0f);
            const float Scaled  = Clamped * 32767.0f;
            const float Rounded = (Scaled >= 0.0f)
                ? FMath::Floor(Scaled + 0.5f)
                : FMath::CeilToFloat(Scaled - 0.5f);
            Dst[i] = static_cast<int16>(Rounded);
        }
        return Out;
    }
}

FInoNeuTTSResult RunSynthesis(
    FInoNeuTTSRunner* Runner,
    const FString& InputText,
    const FInoNeuTTSVoice& Voice,
    const FInoNeuTTSOptions& Options,
    TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CancelFlag)
{
    FInoNeuTTSResult Result;
    Result.SampleRate  = kSampleRate;
    Result.NumChannels = kNumChannels;

    const double T0 = FPlatformTime::Seconds();

    if (!Runner || !Runner->GetEngine() || !Runner->GetDecoder())
    {
        Result.ErrorMessage = TEXT("Runner / engine / decoder not initialized");
        return Result;
    }
    if (IsCancelled(CancelFlag)) { Result.ErrorMessage = TEXT("Cancelled"); return Result; }

    // -----------------------------------------------------------------
    // 1. Ensure the voice is primed in the runner's cache.
    //    Subsystem normally calls SetActiveVoiceAsync to pre-prime;
    //    inline priming is the fallback for callers that skip that.
    // -----------------------------------------------------------------
    if (!Runner->HasCachedVoice(Voice.Name))
    {
        FString PrimeErr;
        if (!Runner->PrimeVoice(Voice, PrimeErr))
        {
            Result.ErrorMessage = FString::Printf(TEXT("PrimeVoice failed: %s"), *PrimeErr);
            return Result;
        }
    }

    // -----------------------------------------------------------------
    // 2. Phonemize the input text in the voice's language.
    // -----------------------------------------------------------------
    if (!UInoSpeakNGBPLibrary::IsAvailable())
    {
        Result.ErrorMessage = TEXT("InoSpeakNG not available — cannot phonemize input text");
        return Result;
    }
    const FString RawInputPhones = UInoSpeakNGBPLibrary::Phonemize(InputText, Voice.Language);
    if (RawInputPhones.IsEmpty())
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("InoSpeakNG.Phonemize returned empty for input text (lang='%s')"),
            *Voice.Language);
        return Result;
    }
    const FString InputPhones = NormalizePhones(RawInputPhones);
    if (IsCancelled(CancelFlag)) { Result.ErrorMessage = TEXT("Cancelled"); return Result; }

    // -----------------------------------------------------------------
    // 3. Build the full prompt — prefix + refphones + " " + inputphones
    //    + suffix + speech-tokens-block.
    // -----------------------------------------------------------------
    const FString FullPrompt = BuildSynthesisPrompt(
        Runner->GetCachedRefPhones(),
        InputPhones,
        Runner->GetCachedSpeechBlock());

    // -----------------------------------------------------------------
    // 4. Drive the backbone — get a list of FSQ codes.
    // -----------------------------------------------------------------
    TArray<int32> SpeechIds;
    FString EngineErr;
    auto CancelCheckFn = [&CancelFlag]() { return IsCancelled(CancelFlag); };

    // MaxNewTokens=0 means "use bundle default" via NULL-like behavior —
    // EngineBackend treats <=0 as "don't override the cap". For now we
    // always pass the user-set value (default 2048 == bundle default).
    if (!Runner->GetEngine()->RunSynthesis(
            FullPrompt, Options.MaxNewTokens, SpeechIds, EngineErr, CancelCheckFn))
    {
        Result.ErrorMessage = FString::Printf(TEXT("backbone failed: %s"), *EngineErr);
        return Result;
    }
    if (SpeechIds.Num() == 0)
    {
        Result.ErrorMessage =
            TEXT("backbone produced 0 speech tokens (model gave a non-speech response)");
        return Result;
    }
    if (IsCancelled(CancelFlag)) { Result.ErrorMessage = TEXT("Cancelled"); return Result; }

    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][Worker] backbone produced %d speech tokens"), SpeechIds.Num());

    // -----------------------------------------------------------------
    // 5. Decode FSQ codes → float32 waveform.
    //    The captured digit from `<|speech_N|>` IS the FSQ code — no
    //    shift needed.
    // -----------------------------------------------------------------
    const int32 MaxBucket = Runner->GetDecoder()->GetMaxBucketFrames();
    if (SpeechIds.Num() > MaxBucket)
    {
        UE_LOG(LogInoAgents, Warning,
            TEXT("[NeuTTS][Worker] %d speech codes exceeds max decoder bucket f%d — truncating. ")
            TEXT("Streaming-decode chunking would lift this cap; see CLAUDE.md roadmap."),
            SpeechIds.Num(), MaxBucket);
        SpeechIds.SetNum(MaxBucket);
    }

    TArray<float> Waveform;
    FString DecErr;
    if (!Runner->GetDecoder()->Decode(SpeechIds, Waveform, DecErr))
    {
        Result.ErrorMessage = FString::Printf(TEXT("decoder failed: %s"), *DecErr);
        return Result;
    }
    if (IsCancelled(CancelFlag)) { Result.ErrorMessage = TEXT("Cancelled"); return Result; }

    // -----------------------------------------------------------------
    // 6. Float32 → int16 PCM LE bytes.
    // -----------------------------------------------------------------
    Result.AudioSamples         = Float32ToInt16PcmBytesMono(Waveform);
    Result.DurationSeconds      = static_cast<float>(Waveform.Num())
                                  / static_cast<float>(kSampleRate);
    Result.GenerationTimeSeconds = static_cast<float>(FPlatformTime::Seconds() - T0);
    Result.RealTimeFactor       = (Result.DurationSeconds > 0.0f)
        ? (Result.GenerationTimeSeconds / Result.DurationSeconds)
        : 0.0f;
    Result.bSuccess = true;

    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][Worker] synth OK: %d samples (%.2fs audio, gen=%.2fs, RTF=%.2fx)"),
        Waveform.Num(), Result.DurationSeconds, Result.GenerationTimeSeconds,
        Result.RealTimeFactor);

    return Result;
}

FInoNeuTTSResult RunStreamingSynthesis(
    FInoNeuTTSRunner* Runner,
    const FString& InputText,
    const FInoNeuTTSVoice& Voice,
    const FInoNeuTTSOptions& Options,
    int32 /*ChunkTokens*/,
    const FStreamChunkFn& OnChunk,
    TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CancelFlag)
{
    // MVP: one-shot synth, then emit the full PCM as a single
    // `bIsFinal=true` chunk. Vendor's `_infer_stream_ggml` chunked-
    // decode-with-overlap-add (test_tts.py reference) is deferred —
    // it requires windowed decoding + linear overlap-add weighting and
    // isn't needed for the first end-to-end synth.
    FInoNeuTTSResult Result = RunSynthesis(Runner, InputText, Voice, Options, CancelFlag);
    if (OnChunk)
    {
        OnChunk(Result.AudioSamples, /*bIsFinal=*/true);
    }
    return Result;
}

} // namespace InoNeuTTSNative
