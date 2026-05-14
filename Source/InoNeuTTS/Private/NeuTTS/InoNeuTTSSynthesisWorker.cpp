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
    // 1. Require the voice to already be primed in the runner's cache.
    //    The subsystem enforces this contract (rejects SynthesizeAsync
    //    when bIsPrimingVoice == true), and the inline-fallback prime
    //    that used to live here is gone because it could race with a
    //    concurrent SetActiveVoiceAsync worker mutating the same cache.
    //    Callers MUST await SetActiveVoiceAsync's OnReady before
    //    invoking synth.
    // -----------------------------------------------------------------
    if (!Runner->HasCachedVoice(Voice.Name))
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Voice '%s' is not primed on the runner. Call ")
            TEXT("UInoNeuTTSSubsystem::SetActiveVoiceAsync and wait for OnReady ")
            TEXT("before SynthesizeAsync."),
            *Voice.Name);
        return Result;
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

// =====================================================================
//  Streaming pipeline — real chunked decode with boundary crossfade.
// =====================================================================
//
// Ports Neuphonic's `_infer_stream_ggml` reference loop. Per "chunk":
//
//   1. AR-loop drives backbone (live, via run_decode_async). The LM
//      callback feeds new <|speech_N|> ids into the queue.
//   2. Once we have enough ids (>= EffectiveChunkTokens + kStreamLookforward
//      + kStreamOverlapFrames), decode a WINDOW that includes lookback
//      context for stable iSTFT.
//   3. Extract the EMIT slice from the window (skip the lookback prefix
//      + the lookforward+overlap suffix unless this is the final flush).
//   4. Append the new slice to a `PendingAudio` buffer with a small
//      linear crossfade against the trailing kStreamOverlapFrames * 480
//      samples of the previous chunk's audio (smooths boundaries that
//      would otherwise click).
//   5. Emit everything in PendingAudio EXCEPT the trailing
//      kStreamOverlapFrames * 480 samples (which might crossfade with
//      the NEXT chunk). On final flush, emit the whole buffer.
//
// Threading: this function runs on a ThreadPool worker. Engine's
// streaming API fires StreamCallback on the LM library's internal
// thread, which calls our captured `OnTokenChunk` lambda. Token-chunk
// processing + decoder dispatch happens INLINE inside that callback —
// LM serializes callbacks so there's no concurrent access to `State`.
// The worker thread blocks waiting for the engine's done event; once
// it returns, it reads State and builds the final FInoNeuTTSResult.

FInoNeuTTSResult RunStreamingSynthesis(
    FInoNeuTTSRunner* Runner,
    const FString& InputText,
    const FInoNeuTTSVoice& Voice,
    const FInoNeuTTSOptions& Options,
    int32 ChunkTokens,
    const FStreamChunkFn& OnChunk,
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
    // 1. Require voice primed (matches one-shot's contract — subsystem
    //    enforces bIsPrimingVoice cross-check upstream).
    // -----------------------------------------------------------------
    if (!Runner->HasCachedVoice(Voice.Name))
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Voice '%s' is not primed. Call SetActiveVoiceAsync and wait for OnReady first."),
            *Voice.Name);
        return Result;
    }

    // -----------------------------------------------------------------
    // 2. Phonemize input.
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
    // 3. Build full prompt (same as one-shot).
    // -----------------------------------------------------------------
    const FString FullPrompt = BuildSynthesisPrompt(
        Runner->GetCachedRefPhones(), InputPhones, Runner->GetCachedSpeechBlock());

    // -----------------------------------------------------------------
    // 4. Set up streaming state. Live on the worker stack; mutated only
    //    inside the LM callback (single-threaded).
    // -----------------------------------------------------------------
    const int32 EffectiveChunkTokens = (ChunkTokens > 0) ? ChunkTokens : kStreamDefaultChunkTokens;
    const int32 OverlapSamples       = kStreamOverlapFrames * kCodecHopLength;
    const int32 Threshold            = EffectiveChunkTokens + kStreamLookforward + kStreamOverlapFrames;
    const int32 MaxDecoderBucket     = Runner->GetDecoder()->GetMaxBucketFrames();

    struct FStreamState
    {
        TArray<int32> AllIds;
        int32         LastEmittedTokenIndex = 0;
        TArray<float> PendingAudio;          // queued audio, last OverlapSamples are not yet finalized
        TArray<int16> AllEmittedSamples;     // concatenated for final Result
        bool          bDecodeError = false;
        FString       DecodeErr;
        int32         NumChunksEmitted = 0;
    };
    FStreamState State;

    auto MaybeDecodeAndEmit = [&](bool bIsFinal)
    {
        if (State.bDecodeError) return;

        while (true)
        {
            const int32 TotalIds          = State.AllIds.Num();
            const int32 NewSinceLastEmit  = TotalIds - State.LastEmittedTokenIndex;
            const bool  bHaveEnough       = (NewSinceLastEmit >= Threshold);
            const bool  bFinalFlush       = bIsFinal && (NewSinceLastEmit > 0);
            if (!bHaveEnough && !bFinalFlush) break;

            // Compute decode window with lookback context.
            const int32 wStart = FMath::Max(0, State.LastEmittedTokenIndex - kStreamLookback);
            int32 wEnd;
            if (bIsFinal)
            {
                wEnd = TotalIds;
            }
            else
            {
                wEnd = State.LastEmittedTokenIndex
                     + EffectiveChunkTokens + kStreamLookforward + kStreamOverlapFrames;
                wEnd = FMath::Min(wEnd, TotalIds);
            }
            // Decoder needs >= 2 frames to produce any output.
            if (wEnd - wStart < 2) break;
            // Hard cap on bucket size.
            if (wEnd - wStart > MaxDecoderBucket)
            {
                wEnd = wStart + MaxDecoderBucket;
            }

            TArrayView<const int32> WindowIds(
                State.AllIds.GetData() + wStart, wEnd - wStart);
            TArray<float> WindowAudio;
            if (!Runner->GetDecoder()->Decode(WindowIds, WindowAudio, State.DecodeErr))
            {
                State.bDecodeError = true;
                UE_LOG(LogInoAgents, Error,
                    TEXT("[NeuTTS][StreamWorker] decoder failed: %s"), *State.DecodeErr);
                return;
            }

            // Extract emit slice.
            const int32 LookbackFrames = State.LastEmittedTokenIndex - wStart;
            const int32 EmitStartSample = LookbackFrames * kCodecHopLength;
            int32 EmitEndSample;
            int32 TokensToAdvance;
            const bool bThisIsTheLast = bIsFinal && (wEnd == TotalIds);
            if (bThisIsTheLast)
            {
                EmitEndSample   = WindowAudio.Num();
                TokensToAdvance = TotalIds - State.LastEmittedTokenIndex;
            }
            else
            {
                EmitEndSample = EmitStartSample + EffectiveChunkTokens * kCodecHopLength;
                EmitEndSample = FMath::Min(EmitEndSample, WindowAudio.Num());
                TokensToAdvance = EffectiveChunkTokens;
            }
            const int32 SliceLen = EmitEndSample - EmitStartSample;
            if (SliceLen <= 0) break;
            TArrayView<const float> NewSlice(
                WindowAudio.GetData() + EmitStartSample, SliceLen);

            // Append to PendingAudio with optional boundary crossfade.
            if (State.PendingAudio.Num() >= OverlapSamples && SliceLen >= OverlapSamples)
            {
                // Linear crossfade: blend the LAST OverlapSamples of
                // PendingAudio with the FIRST OverlapSamples of NewSlice.
                const int32 BlendStart = State.PendingAudio.Num() - OverlapSamples;
                for (int32 s = 0; s < OverlapSamples; ++s)
                {
                    const float W = static_cast<float>(s + 1) /
                                    static_cast<float>(OverlapSamples + 1);
                    State.PendingAudio[BlendStart + s] =
                        State.PendingAudio[BlendStart + s] * (1.0f - W)
                      + NewSlice[s] * W;
                }
                // Append the rest of NewSlice (skip the blended prefix).
                State.PendingAudio.Append(NewSlice.GetData() + OverlapSamples,
                                          SliceLen - OverlapSamples);
            }
            else
            {
                State.PendingAudio.Append(NewSlice.GetData(), SliceLen);
            }

            // Decide how much to emit. On non-final iterations, hold back
            // the trailing OverlapSamples so the NEXT chunk can crossfade.
            int32 EmitLen;
            if (bThisIsTheLast)
            {
                EmitLen = State.PendingAudio.Num();
            }
            else
            {
                EmitLen = State.PendingAudio.Num() - OverlapSamples;
                if (EmitLen < 0) EmitLen = 0;
            }

            if (EmitLen > 0)
            {
                TArrayView<const float> EmitView(State.PendingAudio.GetData(), EmitLen);
                TArray<uint8> ChunkPcmBytes = Float32ToInt16PcmBytesMono(EmitView);
                const int16* AsInt16 = reinterpret_cast<const int16*>(ChunkPcmBytes.GetData());
                State.AllEmittedSamples.Append(AsInt16, EmitLen);

                if (OnChunk)
                {
                    OnChunk(ChunkPcmBytes, bThisIsTheLast);
                }
                State.NumChunksEmitted++;

                State.PendingAudio.RemoveAt(0, EmitLen, EAllowShrinking::No);
            }

            State.LastEmittedTokenIndex += TokensToAdvance;
            if (bThisIsTheLast) break;
        }
    };

    // -----------------------------------------------------------------
    // 5. Drive backbone with the streaming token-chunk callback.
    // -----------------------------------------------------------------
    auto OnTokenChunk = [&](TArrayView<const int32> NewIds, bool bIsFinal)
    {
        if (NewIds.Num() > 0)
        {
            State.AllIds.Append(NewIds.GetData(), NewIds.Num());
        }
        MaybeDecodeAndEmit(bIsFinal);
    };

    FString EngineErr;
    auto CancelCheckFn = [&CancelFlag]() { return IsCancelled(CancelFlag); };
    const bool bEngineOk = Runner->GetEngine()->RunStreamingSynthesis(
        FullPrompt, Options.MaxNewTokens,
        MoveTemp(OnTokenChunk), EngineErr, CancelCheckFn);

    if (State.bDecodeError)
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("decoder failed mid-stream: %s"), *State.DecodeErr);
        // Still expose what audio we emitted before the failure.
    }
    else if (!bEngineOk)
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("backbone streaming failed: %s"), *EngineErr);
    }
    else if (State.AllIds.Num() == 0)
    {
        Result.ErrorMessage = TEXT("backbone produced 0 speech tokens");
    }

    // -----------------------------------------------------------------
    // 6. Build Result from concatenated int16 buffer (regardless of
    //    error — partial audio is useful for caller's UI).
    // -----------------------------------------------------------------
    Result.AudioSamples.SetNumUninitialized(
        State.AllEmittedSamples.Num() * sizeof(int16));
    if (State.AllEmittedSamples.Num() > 0)
    {
        FMemory::Memcpy(Result.AudioSamples.GetData(),
                        State.AllEmittedSamples.GetData(),
                        Result.AudioSamples.Num());
    }
    Result.DurationSeconds       = static_cast<float>(State.AllEmittedSamples.Num())
                                   / static_cast<float>(kSampleRate);
    Result.GenerationTimeSeconds = static_cast<float>(FPlatformTime::Seconds() - T0);
    Result.RealTimeFactor        = (Result.DurationSeconds > 0.0f)
        ? (Result.GenerationTimeSeconds / Result.DurationSeconds)
        : 0.0f;
    Result.bSuccess = Result.ErrorMessage.IsEmpty();

    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][StreamWorker] %s: %d chunks, %d samples (%.2fs audio, gen=%.2fs, RTF=%.2fx)"),
        Result.bSuccess ? TEXT("OK") : TEXT("FAIL"),
        State.NumChunksEmitted, State.AllEmittedSamples.Num(),
        Result.DurationSeconds, Result.GenerationTimeSeconds, Result.RealTimeFactor);

    return Result;
}

} // namespace InoNeuTTSNative
