// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxRunner.h"

#include "InoChatterboxModels.h"
#include "InoChatterboxTokenizer.h"

#include "InoAgentsLog.h"
#include "Onnx/InoOnnxSession.h"
#include "Onnx/InoOnnxTensor.h"
#include "Onnx/InoOnnxTypes.h"

#include "HAL/PlatformTime.h"
#include "Math/UnrealMathUtility.h"

// ============================================================================
// Chatterbox Turbo architecture constants.
//
// These are the official Resemble AI constants for the Turbo variant.
// Verified against the Turbo ONNX bundle at
// ResembleAI/chatterbox-turbo-ONNX and the model card's published
// reference inference script. Same values across every quantization
// variant (fp32 / fp16 / q4 / q4f16 / quantized) — the quantization
// only affects data representation, not model topology.
//
// Do NOT copy these when integrating the original non-Turbo or
// multilingual variants: layer count differs (original is 30-layer;
// multilingual adds language tokens), and the speech-token vocab
// range may also move.
// ============================================================================

namespace
{
    constexpr int64 kStartSpeechToken        = 6561;
    constexpr int64 kStopSpeechToken         = 6562;
    constexpr int64 kSilenceSpeechToken      = 4299;

    constexpr int32 kNumLayers               = 24;   // Turbo 350M
    constexpr int32 kNumKVHeads              = 16;
    constexpr int32 kHeadDim                 = 64;
    constexpr int32 kSpeechVocabSize         = 6563;

    constexpr int32 kSampleRate              = 24000;

    /**
     * Build initial zero-length past_key_values tensors for the language
     * model session, matching the dtype each input actually declares.
     * Exact port of the official reference Python:
     *
     *     past_key_values = {
     *         i.name: np.zeros([batch, NUM_KV_HEADS, 0, HEAD_DIM],
     *                          dtype=np.float16 if i.type == 'tensor(float16)'
     *                                           else np.float32)
     *         for i in language_model_session.get_inputs()
     *         if "past_key_values" in i.name
     *     }
     */
    TArray<FInoOnnxTensor> MakeZeroPastKeyValues(const FInoOnnxSession& LMSess)
    {
        TArray<FInoOnnxTensor> PastKV;
        const TArray<int64> ZeroShape = { 1, kNumKVHeads, 0, kHeadDim };
        const int32 NumInputs = LMSess.GetInputCount();
        PastKV.Reserve(NumInputs);
        for (int32 i = 0; i < NumInputs; ++i)
        {
            const FString Name = LMSess.GetInputName(i);
            if (!Name.Contains(TEXT("past_key_values"))) { continue; }
            const EInoOnnxDtype Dtype = LMSess.GetInputDtype(i);
            PastKV.Add(FInoOnnxTensor::Create(Dtype, ZeroShape));
        }
        return PastKV;
    }

    /**
     * Concatenate two fp32 [1, S, D] tensors along axis 1. Both tensors
     * must have identical batch and D dimensions; result is
     * [1, SA + SB, D]. Batch=1 assumption is baked in — the Chatterbox
     * pipeline doesn't batch and the math stays simple.
     */
    FInoOnnxTensor ConcatFloat32Axis1_B1(
        const FInoOnnxTensor& A,
        const FInoOnnxTensor& B,
        FString* OutError)
    {
        auto Fail = [&](const FString& Msg) -> FInoOnnxTensor
        {
            if (OutError) { *OutError = Msg; }
            return FInoOnnxTensor{};
        };

        const TArray<int64>& SA = A.GetShape();
        const TArray<int64>& SB = B.GetShape();
        if (SA.Num() != 3 || SB.Num() != 3)
        {
            return Fail(TEXT("ConcatFloat32Axis1_B1: both tensors must be 3D"));
        }
        if (SA[0] != 1 || SB[0] != 1)
        {
            return Fail(TEXT("ConcatFloat32Axis1_B1: batch must be 1"));
        }
        if (SA[2] != SB[2])
        {
            return Fail(TEXT("ConcatFloat32Axis1_B1: last-axis dims differ"));
        }
        if (A.GetDtype() != EInoOnnxDtype::Float32 || B.GetDtype() != EInoOnnxDtype::Float32)
        {
            return Fail(TEXT("ConcatFloat32Axis1_B1: inputs must be fp32"));
        }

        const int64 SumS = SA[1] + SB[1];
        const int64 D    = SA[2];

        FInoOnnxTensor Out = FInoOnnxTensor::Create(EInoOnnxDtype::Float32, { 1, SumS, D });
        if (!Out.IsValid()) { return Fail(TEXT("ConcatFloat32Axis1_B1: alloc failed")); }

        float* O = Out.GetMutableData<float>();
        const float* APtr = A.GetData<float>();
        const float* BPtr = B.GetData<float>();
        if (!O || !APtr || !BPtr)
        {
            return Fail(TEXT("ConcatFloat32Axis1_B1: data ptr failure"));
        }
        FMemory::Memcpy(O,               APtr, (SIZE_T)(SA[1] * D) * sizeof(float));
        FMemory::Memcpy(O + SA[1] * D,   BPtr, (SIZE_T)(SB[1] * D) * sizeof(float));
        return Out;
    }

    /**
     * Apply HF-reference repetition penalty in place on a last-token
     * logit slice. For every unique token id the model has generated so
     * far, dampen its logit:
     *   positive logits -> divide by penalty (less attractive)
     *   negative logits -> multiply by penalty (more repelled)
     * Duplicates apply at most once (matches numpy's put_along_axis
     * overwriting-the-same-slot semantics).
     */
    void ApplyRepetitionPenalty(
        float*              Scores,
        int32               VocabSize,
        const TArray<int64>& GeneratedTokens,
        float                Penalty)
    {
        if (Penalty == 1.0f) { return; }
        TSet<int64> Applied;
        Applied.Reserve(GeneratedTokens.Num());
        for (const int64 Id : GeneratedTokens)
        {
            if (Id < 0 || Id >= (int64)VocabSize) { continue; }
            bool bAlreadyIn = false;
            Applied.Add(Id, &bAlreadyIn);
            if (bAlreadyIn) { continue; }
            float& v = Scores[(int32)Id];
            v = (v < 0.0f) ? (v * Penalty) : (v / Penalty);
        }
    }
}

// ============================================================================
// FInoChatterboxRunner
// ============================================================================

FInoChatterboxRunner::FInoChatterboxRunner(
    const FInoChatterboxModels&    InModels,
    const FInoChatterboxTokenizer& InTokenizer)
    : Models(InModels)
    , Tokenizer(InTokenizer)
{
}

bool FInoChatterboxRunner::SynthesizeText(
    const FString&              Text,
    TArrayView<const float>     ReferenceAudio,
    const FSynthesisOptions&    Options,
    FSynthesisResult&           OutResult,
    FString*                    OutError,
    const TAtomic<bool>*        Cancel) const
{
    auto Fail = [&](const FString& Msg) -> bool
    {
        if (OutError) { *OutError = Msg; }
        UE_LOG(LogInoAgents, Error, TEXT("FInoChatterboxRunner: %s"), *Msg);
        return false;
    };

    OutResult = FSynthesisResult{};

    FInoOnnxSession* const EncoderSess = Models.GetSpeechEncoder();
    FInoOnnxSession* const EmbedSess   = Models.GetEmbedTokens();
    FInoOnnxSession* const LMSess      = Models.GetLanguageModel();
    FInoOnnxSession* const DecoderSess = Models.GetConditionalDecoder();
    if (!EncoderSess || !EmbedSess || !LMSess || !DecoderSess)
    {
        return Fail(TEXT("SynthesizeText: one or more sessions in the bundle are null"));
    }

    if (Text.IsEmpty())
    {
        return Fail(TEXT("SynthesizeText: Text is empty"));
    }
    if (ReferenceAudio.Num() == 0)
    {
        return Fail(TEXT("SynthesizeText: ReferenceAudio is empty"));
    }

    const int32 ClampedMaxNewTokens = FMath::Clamp(Options.MaxNewTokens, 1, 1024);

    const double TStart = FPlatformTime::Seconds();
    FString InternalErr;

    // ------------------------------------------------------------------
    // 1. Tokenize the user text
    // ------------------------------------------------------------------
    const TArray<int64> InputIdsInitial = Tokenizer.Encode(Text);
    if (InputIdsInitial.Num() == 0)
    {
        return Fail(TEXT("SynthesizeText: tokenizer produced 0 tokens"));
    }

    // ------------------------------------------------------------------
    // 2. speech_encoder: reference audio → conditioning tensors (once)
    //
    // Output order (positional per the Turbo graph):
    //   [0] cond_emb / audio_features   fp32 [1, C, hidden]
    //   [1] prompt_token / audio_tokens i64  [1, P]
    //   [2] speaker_embeddings           fp32 [1, spk_dim]
    //   [3] speaker_features             fp32 [1, F, feat_dim]
    // ------------------------------------------------------------------
    FInoOnnxTensor AudioInput = FInoOnnxTensor::CreateFromBufferCopy<float>(
        { 1, (int64)ReferenceAudio.Num() }, ReferenceAudio);
    if (!AudioInput.IsValid())
    {
        return Fail(TEXT("SynthesizeText: failed to build audio_values tensor"));
    }
    TArray<FInoOnnxTensor> EncInputs;
    EncInputs.Add(MoveTemp(AudioInput));

    const double EncT0 = FPlatformTime::Seconds();
    TArray<FInoOnnxTensor> EncOutputs;
    if (!EncoderSess->Run(EncInputs, EncOutputs, &InternalErr))
    {
        return Fail(FString::Printf(TEXT("speech_encoder Run failed: %s"), *InternalErr));
    }
    OutResult.EncoderMs = (FPlatformTime::Seconds() - EncT0) * 1000.0;

    if (EncOutputs.Num() != 4)
    {
        return Fail(FString::Printf(
            TEXT("speech_encoder returned %d outputs, expected 4"), EncOutputs.Num()));
    }
    FInoOnnxTensor CondEmb           = MoveTemp(EncOutputs[0]);
    FInoOnnxTensor PromptTokens      = MoveTemp(EncOutputs[1]);
    FInoOnnxTensor SpeakerEmbeddings = MoveTemp(EncOutputs[2]);
    FInoOnnxTensor SpeakerFeatures   = MoveTemp(EncOutputs[3]);

    if (CondEmb.GetShape().Num() != 3 || PromptTokens.GetShape().Num() != 2)
    {
        return Fail(TEXT("speech_encoder outputs have unexpected ranks"));
    }
    const int64 CondLen   = CondEmb.GetShape()[1];
    const int64 PromptLen = PromptTokens.GetShape()[1];

    // ------------------------------------------------------------------
    // 3. AR loop
    //
    // Port of the official reference script's generation loop. Step by
    // step: embed input_ids → (iter 0: prepend cond_emb) → language_model
    // with KV cache → repetition penalty → argmax → stop check → roll
    // state forward.
    // ------------------------------------------------------------------
    TArray<int64> InputIds = InputIdsInitial;
    const int64 TextLen   = (int64)InputIds.Num();
    int64 CurSeqLen       = CondLen + TextLen;  // LM positions seen so far

    TArray<FInoOnnxTensor> PastKV = MakeZeroPastKeyValues(*LMSess);
    if (PastKV.Num() != 2 * kNumLayers)
    {
        return Fail(FString::Printf(
            TEXT("language_model declared %d past_key_values inputs, expected %d"),
            PastKV.Num(), 2 * kNumLayers));
    }

    const int32 ExpectedLMOutputs = 1 + 2 * kNumLayers;
    const int32 NumLMInputs       = LMSess->GetInputCount();

    TArray<int64> GeneratedTokens;
    GeneratedTokens.Reserve(ClampedMaxNewTokens + 2);
    GeneratedTokens.Add(kStartSpeechToken);

    for (int32 Iter = 0; Iter < ClampedMaxNewTokens; ++Iter)
    {
        // Cooperative cancel check — sampled at the top of every AR
        // iteration so we bail before starting ~tens of ms of ORT Runs.
        // Relaxed load: the caller's writes to this flag don't need to
        // synchronize with anything other than eventually being seen
        // (no data is published through it).
        if (Cancel && Cancel->Load(EMemoryOrder::Relaxed))
        {
            return Fail(TEXT("SynthesizeText: cancelled"));
        }

        // --- Embed current input_ids ---
        const int64 CurInputLen = InputIds.Num();
        FInoOnnxTensor EmbedInput = FInoOnnxTensor::CreateFromBufferCopy<int64>(
            { 1, CurInputLen }, MakeArrayView(InputIds));
        if (!EmbedInput.IsValid())
        {
            return Fail(FString::Printf(TEXT("iter %d: failed to build embed input tensor"), Iter));
        }

        TArray<FInoOnnxTensor> EmbedInputs;
        EmbedInputs.Add(MoveTemp(EmbedInput));

        const double EmbedT0 = FPlatformTime::Seconds();
        TArray<FInoOnnxTensor> EmbedOutputs;
        if (!EmbedSess->Run(EmbedInputs, EmbedOutputs, &InternalErr))
        {
            return Fail(FString::Printf(TEXT("iter %d: embed_tokens Run failed: %s"), Iter, *InternalErr));
        }
        OutResult.EmbedTotalMs += (FPlatformTime::Seconds() - EmbedT0) * 1000.0;
        FInoOnnxTensor TextEmbeds = MoveTemp(EmbedOutputs[0]);

        // --- At iter 0 only: concat cond_emb + text_embeds ---
        FInoOnnxTensor InputsEmbeds;
        if (Iter == 0)
        {
            InputsEmbeds = ConcatFloat32Axis1_B1(CondEmb, TextEmbeds, &InternalErr);
            if (!InputsEmbeds.IsValid())
            {
                return Fail(FString::Printf(TEXT("iter 0 cond+text concat failed: %s"), *InternalErr));
            }
            // cond_emb is consumed — free it immediately.
            CondEmb = FInoOnnxTensor{};
        }
        else
        {
            InputsEmbeds = MoveTemp(TextEmbeds);
        }

        // --- attention_mask: ones, length = cumulative seq ---
        FInoOnnxTensor AttnMask = FInoOnnxTensor::Create(
            EInoOnnxDtype::Int64, { 1, CurSeqLen });
        if (int64* D = AttnMask.GetMutableData<int64>())
        {
            for (int64 i = 0; i < CurSeqLen; ++i) { D[i] = 1; }
        }

        // --- position_ids ---
        //   iter 0:  [0, 1, ..., CurSeqLen-1]       (full arange)
        //   iter >0: [[CurSeqLen - 1]]               (scalar, [1, 1])
        // Matches Python's `position_ids[:, -1:] + 1` pattern.
        FInoOnnxTensor PosIds;
        if (Iter == 0)
        {
            PosIds = FInoOnnxTensor::Create(EInoOnnxDtype::Int64, { 1, CurSeqLen });
            if (int64* D = PosIds.GetMutableData<int64>())
            {
                for (int64 i = 0; i < CurSeqLen; ++i) { D[i] = i; }
            }
        }
        else
        {
            PosIds = FInoOnnxTensor::Create(EInoOnnxDtype::Int64, { 1, 1 });
            if (int64* D = PosIds.GetMutableData<int64>())
            {
                D[0] = CurSeqLen - 1;
            }
        }

        // --- Assemble full LM input bundle: embeds + mask + pos + past ---
        TArray<FInoOnnxTensor> LMInputs;
        LMInputs.Reserve(NumLMInputs);
        LMInputs.Add(MoveTemp(InputsEmbeds));
        LMInputs.Add(MoveTemp(AttnMask));
        LMInputs.Add(MoveTemp(PosIds));
        for (int32 i = 0; i < PastKV.Num(); ++i)
        {
            LMInputs.Add(MoveTemp(PastKV[i]));
        }
        PastKV.Reset();

        // --- Run language_model ---
        const double LMT0 = FPlatformTime::Seconds();
        TArray<FInoOnnxTensor> LMOutputs;
        if (!LMSess->Run(LMInputs, LMOutputs, &InternalErr))
        {
            return Fail(FString::Printf(TEXT("iter %d: language_model Run failed: %s"), Iter, *InternalErr));
        }
        OutResult.LanguageModelMs += (FPlatformTime::Seconds() - LMT0) * 1000.0;

        if (LMOutputs.Num() != ExpectedLMOutputs)
        {
            return Fail(FString::Printf(
                TEXT("iter %d: language_model returned %d outputs, expected %d"),
                Iter, LMOutputs.Num(), ExpectedLMOutputs));
        }

        // --- Extract last-position logits, apply rep penalty, greedy argmax ---
        const FInoOnnxTensor& Logits = LMOutputs[0];
        const TArray<int64>& LogitShape = Logits.GetShape();
        if (LogitShape.Num() != 3 || LogitShape[2] != kSpeechVocabSize
            || Logits.GetDtype() != EInoOnnxDtype::Float32)
        {
            return Fail(FString::Printf(
                TEXT("iter %d: unexpected logits shape or dtype"), Iter));
        }
        const int64 LogitSeqLen = LogitShape[1];
        const float* LogitData  = Logits.GetData<float>();
        if (LogitData == nullptr)
        {
            return Fail(FString::Printf(TEXT("iter %d: Logits.GetData<float> returned null"), Iter));
        }

        TArray<float> Scores;
        Scores.SetNumUninitialized(kSpeechVocabSize);
        FMemory::Memcpy(
            Scores.GetData(),
            LogitData + (LogitSeqLen - 1) * kSpeechVocabSize,
            (SIZE_T)kSpeechVocabSize * sizeof(float));
        ApplyRepetitionPenalty(
            Scores.GetData(), kSpeechVocabSize,
            GeneratedTokens, Options.RepetitionPenalty);

        // Greedy argmax — matches np.argmax(scores, axis=-1).
        int64 NextToken = 0;
        float BestScore = -FLT_MAX;
        for (int32 i = 0; i < kSpeechVocabSize; ++i)
        {
            if (Scores[i] > BestScore) { BestScore = Scores[i]; NextToken = i; }
        }

        GeneratedTokens.Add(NextToken);

        if (NextToken == kStopSpeechToken)
        {
            OutResult.bHitStopToken = true;
            break;
        }

        // --- Roll state forward for next iter ---
        InputIds.Reset(1);
        InputIds.Add(NextToken);
        CurSeqLen += 1;
        for (int32 i = 1; i < LMOutputs.Num(); ++i)
        {
            PastKV.Add(MoveTemp(LMOutputs[i]));
        }
    }

    OutResult.NumGeneratedTokens = GeneratedTokens.Num() - 1
        - (OutResult.bHitStopToken ? 1 : 0);  // exclude leading START and trailing STOP (if present)

    // ------------------------------------------------------------------
    // 4. Build decoder input: concat(prompt_token, generate[1:-1], silence×3)
    // ------------------------------------------------------------------
    const int32 NumGen = GeneratedTokens.Num();
    if (NumGen < 2)
    {
        return Fail(TEXT("AR loop produced too few tokens to decode"));
    }

    TArray<int64> GenSlice;
    {
        const int32 InnerCount = FMath::Max(0, NumGen - 2);
        GenSlice.Reserve(InnerCount + 3);
        for (int32 i = 1; i < NumGen - 1; ++i)
        {
            GenSlice.Add(GeneratedTokens[i]);
        }
    }
    // Silence tail — required by the reference script. 3 SILENCE_TOKEN.
    for (int32 i = 0; i < 3; ++i) { GenSlice.Add(kSilenceSpeechToken); }

    const int64* PromptData = PromptTokens.GetData<int64>();
    if (PromptData == nullptr)
    {
        return Fail(TEXT("prompt_token GetData<int64> returned null"));
    }

    const int64 TotalSpeechLen = PromptLen + (int64)GenSlice.Num();
    FInoOnnxTensor SpeechTokens = FInoOnnxTensor::Create(
        EInoOnnxDtype::Int64, { 1, TotalSpeechLen });
    if (!SpeechTokens.IsValid())
    {
        return Fail(FString::Printf(TEXT("failed to alloc speech_tokens [%lld]"), TotalSpeechLen));
    }
    if (int64* ST = SpeechTokens.GetMutableData<int64>())
    {
        FMemory::Memcpy(ST, PromptData, (SIZE_T)PromptLen * sizeof(int64));
        if (GenSlice.Num() > 0)
        {
            FMemory::Memcpy(ST + PromptLen, GenSlice.GetData(),
                            (SIZE_T)GenSlice.Num() * sizeof(int64));
        }
    }

    // ------------------------------------------------------------------
    // 5. conditional_decoder: speech_tokens + speaker conditioning → wav
    // ------------------------------------------------------------------
    TArray<FInoOnnxTensor> DecInputs;
    DecInputs.Reserve(3);
    DecInputs.Add(MoveTemp(SpeechTokens));
    DecInputs.Add(MoveTemp(SpeakerEmbeddings));
    DecInputs.Add(MoveTemp(SpeakerFeatures));

    const double DecT0 = FPlatformTime::Seconds();
    TArray<FInoOnnxTensor> DecOutputs;
    if (!DecoderSess->Run(DecInputs, DecOutputs, &InternalErr))
    {
        return Fail(FString::Printf(TEXT("conditional_decoder Run failed: %s"), *InternalErr));
    }
    OutResult.DecoderMs = (FPlatformTime::Seconds() - DecT0) * 1000.0;

    if (DecOutputs.Num() != 1 || !DecOutputs[0].IsValid())
    {
        return Fail(TEXT("conditional_decoder produced no output"));
    }
    const FInoOnnxTensor& Wav = DecOutputs[0];
    const TArray<int64>& WavShape = Wav.GetShape();
    if (WavShape.Num() != 2 || WavShape[0] != 1
        || Wav.GetDtype() != EInoOnnxDtype::Float32)
    {
        return Fail(TEXT("conditional_decoder output has unexpected shape or dtype"));
    }

    const int64 SampleCount = WavShape[1];
    const float* WavData    = Wav.GetData<float>();
    if (WavData == nullptr)
    {
        return Fail(TEXT("conditional_decoder output GetData<float> returned null"));
    }

    OutResult.AudioSamples.SetNumUninitialized((int32)SampleCount);
    FMemory::Memcpy(OutResult.AudioSamples.GetData(), WavData,
                    (SIZE_T)SampleCount * sizeof(float));
    OutResult.SampleRate     = kSampleRate;
    OutResult.TotalElapsedMs = (FPlatformTime::Seconds() - TStart) * 1000.0;

    return true;
}
