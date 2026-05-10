// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxTurboNativeRunner.h"

#include "InoChatterboxTurboNativeDecoderWorker.h"
#include "InoChatterboxTurboNativeModels.h"
#include "InoChatterboxTurboNativeTokenizer.h"

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
// FInoChatterboxTurboNativeRunner
// ============================================================================

FInoChatterboxTurboNativeRunner::FInoChatterboxTurboNativeRunner(
    const FInoChatterboxTurboNativeModels&    InModels,
    const FInoChatterboxTurboNativeTokenizer& InTokenizer)
    : Models(InModels)
    , Tokenizer(InTokenizer)
{
}

bool FInoChatterboxTurboNativeRunner::SynthesizeText(
    const FString&              Text,
    TArrayView<const float>     ReferenceAudio,
    const FSynthesisOptions&    Options,
    FSynthesisResult&           OutResult,
    FString*                    OutError,
    const TAtomic<bool>*        Cancel,
    int32                       StreamChunkTokens,
    const FOnStreamChunk&       OnChunk) const
{
    auto Fail = [&](const FString& Msg) -> bool
    {
        if (OutError) { *OutError = Msg; }
        UE_LOG(LogInoAgents, Error, TEXT("Chatterbox: Runner: %s"), *Msg);
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
    const bool  bWillStream         = OnChunk && StreamChunkTokens > 0;

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Runner: SynthesizeText begin ")
           TEXT("(text_len=%d, ref_audio_samples=%d, max_new_tokens=%d, ")
           TEXT("stream_chunk_tokens=%d, streaming=%s, rep_penalty=%.2f)"),
           Text.Len(), ReferenceAudio.Num(), ClampedMaxNewTokens,
           StreamChunkTokens,
           bWillStream ? TEXT("on") : TEXT("off"),
           Options.RepetitionPenalty);

    const double TStart = FPlatformTime::Seconds();
    FString InternalErr;

    // ------------------------------------------------------------------
    // 1. Tokenize the user text
    // ------------------------------------------------------------------
    const double TokT0 = FPlatformTime::Seconds();
    const TArray<int64> InputIdsInitial = Tokenizer.Encode(Text);
    const double TokMs = (FPlatformTime::Seconds() - TokT0) * 1000.0;
    if (InputIdsInitial.Num() == 0)
    {
        return Fail(TEXT("SynthesizeText: tokenizer produced 0 tokens"));
    }
    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Runner: tokenized input -> %d tokens (%.1f ms)"),
           InputIdsInitial.Num(), TokMs);

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

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Runner: speech_encoder done (%.1f ms) -- cond_len=%lld, prompt_len=%lld"),
           OutResult.EncoderMs, CondLen, PromptLen);

    // Pin the prompt_token data pointer once; the intermediate-chunk
    // path below copies it into a per-chunk IntermediateSpan without
    // mutating the underlying tensor, and the final decode copies from
    // the same pointer into the final concat buffer. Must be fetched
    // before the streaming loop can reference it.
    const int64* PromptData = PromptTokens.GetData<int64>();
    if (PromptData == nullptr)
    {
        return Fail(TEXT("prompt_token GetData<int64> returned null"));
    }

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

    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Runner: AR loop begin (max_iters=%d, stream_chunk_tokens=%d, ")
           TEXT("initial_seq_len=%lld, num_kv_layers=%d)"),
           ClampedMaxNewTokens, StreamChunkTokens, CurSeqLen, kNumLayers);
    const double ARLoopT0 = FPlatformTime::Seconds();

    const int32 ExpectedLMOutputs = 1 + 2 * kNumLayers;
    const int32 NumLMInputs       = LMSess->GetInputCount();

    TArray<int64> GeneratedTokens;
    GeneratedTokens.Reserve(ClampedMaxNewTokens + 2);
    GeneratedTokens.Add(kStartSpeechToken);

    // ------------------------------------------------------------------
    // Streaming setup
    //
    // When StreamChunkTokens > 0 AND OnChunk is set, we run the
    // conditional_decoder periodically during the AR loop (every N new
    // tokens) on the prefix of what's been generated so far, and fire
    // OnChunk with only the NEW samples (delta from the prior decode).
    //
    // The decoder takes three inputs: speech_tokens, speaker_embeddings,
    // speaker_features. The latter two are invariant across calls so we
    // MoveTemp them into slots [1] and [2] of DecInputsStore ONCE, and
    // the speech_tokens slot [0] gets replaced per call. This avoids
    // losing the tensors after the first Run (TArray<FInoOnnxTensor> is
    // move-only) and keeps the decoder setup cost amortized.
    //
    // Regardless of streaming mode, a FINAL decoder call always runs
    // after the AR loop on concat(prompt, generated[1:-1], silence×3).
    // Its delta (vs. the last streamed chunk, or vs. 0 if no streaming)
    // fires OnChunk with bIsFinal=true — that's the one callback every
    // OnChunk-bound consumer is guaranteed to see. Cancellation skips
    // the final call (OnChunk is NOT invoked on cancel).
    // ------------------------------------------------------------------
    const bool bStreamingEnabled =
        OnChunk && StreamChunkTokens > 0;

    TArray<FInoOnnxTensor> DecInputsStore;
    DecInputsStore.AddDefaulted(3);
    DecInputsStore[1] = MoveTemp(SpeakerEmbeddings);
    DecInputsStore[2] = MoveTemp(SpeakerFeatures);

    // Float32 waveform buffer carried across all decoder calls. Grows
    // each chunk; the suffix beyond LastEmittedSampleCount is what the
    // next OnChunk ships.
    TArray<float> StreamAudioBuffer;
    int32         LastEmittedSampleCount = 0;

    // Tokens added since the last intermediate-chunk emit. Resets to 0
    // after each emit. Only meaningful if bStreamingEnabled.
    int32 TokensSinceLastChunk = 0;

    // Reusable helper: run the conditional_decoder on the given
    // speech-token span (already prefixed with PromptTokens by the
    // caller when building the span), store the resulting waveform into
    // StreamAudioBuffer (replacing previous contents), and return true
    // on success. Decoder timing accumulates into OutResult.DecoderMs.
    auto RunDecoder = [&](const TArray<int64>& FullSpeechSpan) -> bool
    {
        const int64 TotalLen = (int64)FullSpeechSpan.Num();
        FInoOnnxTensor SpeechTokens = FInoOnnxTensor::Create(
            EInoOnnxDtype::Int64, { 1, TotalLen });
        if (!SpeechTokens.IsValid())
        {
            InternalErr = FString::Printf(
                TEXT("failed to alloc speech_tokens [%lld]"), TotalLen);
            return false;
        }
        if (int64* ST = SpeechTokens.GetMutableData<int64>())
        {
            FMemory::Memcpy(
                ST, FullSpeechSpan.GetData(),
                (SIZE_T)TotalLen * sizeof(int64));
        }

        DecInputsStore[0] = MoveTemp(SpeechTokens);

        const double DT0 = FPlatformTime::Seconds();
        TArray<FInoOnnxTensor> DecOutputs;
        if (!DecoderSess->Run(DecInputsStore, DecOutputs, &InternalErr))
        {
            return false;
        }
        OutResult.DecoderMs += (FPlatformTime::Seconds() - DT0) * 1000.0;

        if (DecOutputs.Num() != 1 || !DecOutputs[0].IsValid())
        {
            InternalErr = TEXT("conditional_decoder produced no output");
            return false;
        }
        const FInoOnnxTensor& Wav = DecOutputs[0];
        const TArray<int64>& WavShape = Wav.GetShape();
        if (WavShape.Num() != 2 || WavShape[0] != 1
            || Wav.GetDtype() != EInoOnnxDtype::Float32)
        {
            InternalErr = TEXT("conditional_decoder output has unexpected shape or dtype");
            return false;
        }
        const int64 SampleCount = WavShape[1];
        const float* WavData    = Wav.GetData<float>();
        if (WavData == nullptr)
        {
            InternalErr = TEXT("conditional_decoder output GetData<float> returned null");
            return false;
        }

        StreamAudioBuffer.SetNumUninitialized((int32)SampleCount);
        FMemory::Memcpy(
            StreamAudioBuffer.GetData(), WavData,
            (SIZE_T)SampleCount * sizeof(float));
        return true;
    };

    // Reusable helper: given the latest StreamAudioBuffer, emit samples
    // from LastEmittedSampleCount..end via OnChunk. Advances
    // LastEmittedSampleCount. Caller controls bIsFinal.
    //
    // A subtle property worth preserving: each decoder call on a longer
    // prefix may produce slightly different earlier samples than the
    // prior call (full-attention decoder, not strictly position-
    // deterministic). We bet on near-equality (Chatterbox's flow-matching
    // decoder is well-behaved here in practice) and emit only the tail —
    // the alternative of shipping the full buffer each time would force
    // every consumer to diff/dedup, worse ergonomics. Callers noticing
    // artefacts at chunk boundaries can raise StreamChunkTokens to make
    // chunks rarer-and-larger. Zero = disable streaming entirely and
    // get a perfectly clean single-shot decode.
    auto EmitDeltaChunk = [&](int32 NumGenTokens, bool bFinal)
    {
        if (!OnChunk)
        {
            return;
        }
        const int32 Total     = StreamAudioBuffer.Num();
        const int32 PrevTotal = LastEmittedSampleCount;

        // Guard: if this decoder call produced fewer samples than the
        // last emit point, LastEmittedSampleCount would be > Total and
        // we'd otherwise silently drop those trailing samples. In
        // practice the FINAL decode adds silence×3 (~120 ms, >>2 samples)
        // so it's always longer than any preceding intermediate —
        // hitting this branch means either the decoder regressed on a
        // longer input (bug in the model or the tokens we passed) or
        // our LastEmittedSampleCount bookkeeping is off. Log once so
        // the user notices; don't fail the synth.
        if (LastEmittedSampleCount > Total)
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("Chatterbox: Runner: decoder %s output (%d samples) ")
                   TEXT("is SHORTER than already-emitted prefix (%d samples). ")
                   TEXT("%d samples from the stream won't reach the consumer. ")
                   TEXT("Listening-check the waveform; if it's correct, this is ")
                   TEXT("harmless decoder-length drift -- if it's clipped, investigate."),
                   bFinal ? TEXT("final") : TEXT("intermediate"),
                   Total, LastEmittedSampleCount,
                   LastEmittedSampleCount - Total);
        }

        const int32 NewStart  = FMath::Clamp(LastEmittedSampleCount, 0, Total);
        const int32 NewLen    = Total - NewStart;
        // Guard: never emit an empty non-final chunk (pointless),
        // but ALWAYS emit the final callback even if zero-length so
        // consumers have a deterministic "done" signal.
        if (NewLen > 0 || bFinal)
        {
            OnChunk(
                MakeArrayView(
                    StreamAudioBuffer.GetData() + NewStart,
                    FMath::Max(0, NewLen)),
                NumGenTokens,
                bFinal);
            if (!bFinal)
            {
                UE_LOG(LogInoAgents, Log,
                       TEXT("Chatterbox: Runner: intermediate decode %d tokens -> %d total samples ")
                       TEXT("(delta=%d samples, prev_emitted=%d)"),
                       NumGenTokens, Total, FMath::Max(0, NewLen), PrevTotal);
            }
            LastEmittedSampleCount = Total;
        }
    };

    // ------------------------------------------------------------------
    // Parallel decoder worker
    //
    // When streaming is enabled, spawn a background thread to run
    // intermediate decodes. The AR loop only *publishes* (non-blocking)
    // prefix-token requests; the worker thread runs the decoder + fires
    // OnChunk. Because the LM and decoder are independent ORT sessions,
    // they genuinely execute in parallel — the AR loop isn't stalled by
    // decoder wallclock, and the total streaming synth time drops from
    // roughly "LM + Σ(intermediate decode cost) + final decode" to
    // roughly "max(LM, Σ decode) + final decode". For typical utterances
    // (LM > total decode), streaming cost is effectively hidden behind
    // the LM forward passes.
    //
    // Shared state between AR and decoder threads:
    //   DecInputsStore[0]       — rewritten by RunDecoder, read by ORT
    //   StreamAudioBuffer       — rewritten by RunDecoder
    //   LastEmittedSampleCount  — advanced by EmitDeltaChunk
    //   OutResult.DecoderMs     — += in RunDecoder
    //
    // None of these are concurrency-safe. The invariant we rely on:
    //   - The decoder thread runs RunDecoder + EmitDeltaChunk for
    //     intermediates ONLY.
    //   - The AR thread runs RunDecoder + EmitDeltaChunk for the FINAL
    //     decode ONLY, and calls DecoderWorker->WaitForIdle() first.
    //   - Single-slot queue means at most one Task is in flight on the
    //     worker thread at any moment (no inter-Task overlap).
    // => No two writers ever touch the shared state simultaneously.
    //
    // Cancel semantics: the decoder worker checks Cancel inside its
    // Task before calling EmitDeltaChunk, so a chunk that was decoded
    // after the cancel flipped does NOT emit — honouring the "OnChunk
    // is not invoked on cancel" contract (a chunk decoded before the
    // cancel flipped emits normally; the AR loop observes cancel and
    // Fail()s before the final decode).
    // ------------------------------------------------------------------
    TUniquePtr<FInoChatterboxTurboNativeDecoderWorker> DecoderWorker;
    if (bStreamingEnabled)
    {
        DecoderWorker = MakeUnique<FInoChatterboxTurboNativeDecoderWorker>(
            [&](const TArray<int64>& Tokens,
                int32                NumGenTokensSnapshot,
                FString&             OutErr) -> bool
            {
                if (!RunDecoder(Tokens))
                {
                    OutErr = InternalErr;
                    return false;
                }
                // Drop the chunk silently if cancel flipped during the
                // decode — the AR loop's next iteration check will
                // observe cancel and Fail() the synth, and we don't
                // want a stray OnChunk fire after that error callback.
                if (Cancel && Cancel->Load(EMemoryOrder::Relaxed))
                {
                    UE_LOG(LogInoAgents, Warning,
                           TEXT("Chatterbox: Decoder: Cancel observed -- skipping emit"));
                    return true;
                }
                EmitDeltaChunk(NumGenTokensSnapshot, /*bFinal=*/ false);
                return true;
            });
    }

    for (int32 Iter = 0; Iter < ClampedMaxNewTokens; ++Iter)
    {
        // Cooperative cancel check — sampled at the top of every AR
        // iteration so we bail before starting ~tens of ms of ORT Runs.
        // Relaxed load: the caller's writes to this flag don't need to
        // synchronize with anything other than eventually being seen
        // (no data is published through it).
        if (Cancel && Cancel->Load(EMemoryOrder::Relaxed))
        {
            const double ARCancelMs = (FPlatformTime::Seconds() - ARLoopT0) * 1000.0;
            UE_LOG(LogInoAgents, Warning,
                   TEXT("Chatterbox: Runner: AR loop CANCELLED at iter %d (%.1f ms in AR loop, ")
                   TEXT("%d tokens generated)"),
                   Iter, ARCancelMs, GeneratedTokens.Num() - 1);
            return Fail(TEXT("SynthesizeText: cancelled"));
        }

        // Periodic progress Log at every 20 iterations — plus a
        // per-iteration Verbose line. The periodic log is the single
        // most useful line for diagnosing "got stuck somewhere in the
        // AR loop" without needing to enable Verbose.
        if ((Iter % 20) == 0 && Iter > 0)
        {
            const double ARElapsedS = FPlatformTime::Seconds() - ARLoopT0;
            UE_LOG(LogInoAgents, Log,
                   TEXT("Chatterbox: Runner: AR iter %d/%d (tokens_since_last_chunk=%d, ")
                   TEXT("elapsed=%.1f s)"),
                   Iter, ClampedMaxNewTokens, TokensSinceLastChunk, ARElapsedS);
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

        UE_LOG(LogInoAgents, Verbose,
               TEXT("Chatterbox: Runner: AR iter %d -- next_token=%lld (cur_seq_len=%lld, best_score=%.3f)"),
               Iter, NextToken, CurSeqLen, BestScore);

        if (NextToken == kStopSpeechToken)
        {
            OutResult.bHitStopToken = true;
            const double ARHitStopMs = (FPlatformTime::Seconds() - ARLoopT0) * 1000.0;
            // This is the critical diagnostic line for "voice generating
            // but not completely" — if we never see this log, we didn't
            // hit STOP and instead ran to MaxNewTokens (or were cancelled).
            UE_LOG(LogInoAgents, Log,
                   TEXT("Chatterbox: Runner: AR loop hit STOP at iter %d (%.1f ms total, ")
                   TEXT("%d speech tokens generated)"),
                   Iter, ARHitStopMs, GeneratedTokens.Num() - 2);
            break;
        }

        // --- Streaming: publish an intermediate decode every N tokens ---
        //
        // Non-blocking. The AR thread only builds concat(PromptTokens,
        // generated[1:]) and hands it to the decoder worker; the
        // worker's thread runs the ORT decoder Run and fires OnChunk.
        // This lets the LM keep generating tokens while the decoder
        // is busy — the two ORT sessions execute in parallel.
        //
        // Single-slot semantics: if the LM is fast enough to fire a
        // second chunk boundary before the worker has picked up the
        // first, the first is silently discarded (worker always takes
        // the latest). Consumer sees fewer chunks than (total_tokens /
        // StreamChunkTokens) would suggest in that pathological case,
        // but the chunks they see are always current.
        if (bStreamingEnabled)
        {
            ++TokensSinceLastChunk;
            if (TokensSinceLastChunk >= StreamChunkTokens)
            {
                TokensSinceLastChunk = 0;

                // Build concat(PromptTokens, generated[1:]). Skip index 0
                // (START); include every non-STOP token we've emitted.
                const int32 NumGenSoFar = GeneratedTokens.Num();
                const int32 GenTailLen  = FMath::Max(0, NumGenSoFar - 1);
                TArray<int64> IntermediateSpan;
                IntermediateSpan.Reserve((int32)PromptLen + GenTailLen);
                for (int64 i = 0; i < PromptLen; ++i)
                {
                    IntermediateSpan.Add(PromptData[i]);
                }
                for (int32 i = 1; i < NumGenSoFar; ++i)
                {
                    IntermediateSpan.Add(GeneratedTokens[i]);
                }

                // Tokens generated so far, excluding the leading START.
                // STOP is not in GeneratedTokens yet at this point (it
                // would have broken out of the loop above).
                const int32 TokensSoFar = GeneratedTokens.Num() - 1;

                UE_LOG(LogInoAgents, Log,
                       TEXT("Chatterbox: Runner: publishing intermediate decode at iter %d ")
                       TEXT("(tokens_so_far=%d, span_len=%d)"),
                       Iter, TokensSoFar, (int32)PromptLen + GenTailLen);

                // Publish and continue — worker runs decoder + emits
                // OnChunk on its own thread. No block here.
                DecoderWorker->Publish(MoveTemp(IntermediateSpan), TokensSoFar);
            }
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

    // If we didn't hit STOP, we hit MaxNewTokens. Log distinctly — this
    // is the other critical "voice generating but not completely" case:
    // the utterance was truncated before the model said it was done.
    // A caller seeing this should either raise MaxNewTokens or accept
    // that the LM is being asked to say more than it's been trained for.
    if (!OutResult.bHitStopToken)
    {
        const double ARMaxMs = (FPlatformTime::Seconds() - ARLoopT0) * 1000.0;
        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox: Runner: AR loop hit max_new_tokens %d (%.1f ms total, did NOT hit STOP) ")
               TEXT("-- utterance will be TRUNCATED. Raise MaxNewTokens if the audio ends abruptly."),
               ClampedMaxNewTokens, ARMaxMs);
    }

    // ------------------------------------------------------------------
    // Drain the parallel decoder before the final decode
    //
    // The AR loop may have published one or more intermediate decode
    // requests that the worker hasn't picked up / finished yet. We must
    // wait for the worker to become idle before running the final
    // decode on this thread, because the final decode writes into the
    // same DecInputsStore[0] + StreamAudioBuffer that the worker's
    // Task is writing to. WaitForIdle() is a mutex+event block —
    // typically returns immediately or waits at most one decoder Run().
    //
    // After waiting, check for a latched Task error. Any intermediate
    // decode that failed surfaces here so the caller sees the same
    // failure signal they'd have gotten from the synchronous path.
    // ------------------------------------------------------------------
    if (DecoderWorker.IsValid())
    {
        const double WaitT0 = FPlatformTime::Seconds();
        DecoderWorker->WaitForIdle();
        const double WaitMs = (FPlatformTime::Seconds() - WaitT0) * 1000.0;
        UE_LOG(LogInoAgents, Verbose,
               TEXT("Chatterbox: Runner: drained parallel decoder before final decode (waited %.1f ms)"),
               WaitMs);
        FString DecErr;
        if (DecoderWorker->HasError(DecErr))
        {
            return Fail(FString::Printf(
                TEXT("parallel decoder worker reported error: %s"), *DecErr));
        }
    }

    // ------------------------------------------------------------------
    // 4. Final decode: concat(prompt_token, generate[1:-1], silence×3)
    //
    // Always runs exactly once, regardless of streaming mode. Its output
    // is authoritative for OutResult.AudioSamples. When streaming was
    // enabled, the per-chunk EmitDeltaChunk above has already shipped
    // some audio — the final EmitDeltaChunk(bFinal=true) ships the
    // trailing suffix (the silence + any slightly-drifted regenerated
    // samples beyond what was emitted), so consumers see the complete
    // waveform once they've concatenated every chunk.
    // ------------------------------------------------------------------
    const int32 NumGen = GeneratedTokens.Num();
    if (NumGen < 2)
    {
        return Fail(TEXT("AR loop produced too few tokens to decode"));
    }

    // Build concat(PromptTokens, generated[1:-1], silence×3) as a single
    // int64 span the RunDecoder helper can copy into a speech_tokens
    // tensor. GenSlice is just generated[1:-1]; FinalSpan prepends the
    // prompt and appends silence.
    TArray<int64> FinalSpan;
    {
        const int32 InnerCount = FMath::Max(0, NumGen - 2);
        FinalSpan.Reserve((int32)PromptLen + InnerCount + 3);
        for (int64 i = 0; i < PromptLen; ++i)
        {
            FinalSpan.Add(PromptData[i]);
        }
        for (int32 i = 1; i < NumGen - 1; ++i)
        {
            FinalSpan.Add(GeneratedTokens[i]);
        }
        // Silence tail — required by the reference script. 3 SILENCE_TOKEN.
        for (int32 i = 0; i < 3; ++i) { FinalSpan.Add(kSilenceSpeechToken); }
    }

    const double FinalDecT0 = FPlatformTime::Seconds();
    const int32  FinalSpanLen = FinalSpan.Num();
    if (!RunDecoder(FinalSpan))
    {
        return Fail(FString::Printf(
            TEXT("conditional_decoder Run failed: %s"), *InternalErr));
    }
    const double FinalDecMs   = (FPlatformTime::Seconds() - FinalDecT0) * 1000.0;
    const int32  FinalSamples = StreamAudioBuffer.Num();
    const int32  PrevEmitted  = LastEmittedSampleCount;
    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Runner: final decode %d tokens -> %d samples (%.1f ms)"),
           FinalSpanLen, FinalSamples, FinalDecMs);

    // Fire the terminal chunk. When streaming was enabled this ships
    // the trailing delta; when it was disabled this ships the full
    // waveform. Either way, OnChunk(bFinal=true) is the single signal
    // every streaming consumer binds to, so this fires regardless of
    // whether StreamChunkTokens was zero or not — but only if OnChunk
    // is set (empty TFunction = no-op in the helper).
    EmitDeltaChunk(OutResult.NumGeneratedTokens, /*bFinal=*/ true);

    if (OnChunk)
    {
        const int32 FinalDelta = FMath::Max(0, FinalSamples - PrevEmitted);
        UE_LOG(LogInoAgents, Log,
               TEXT("Chatterbox: Runner: final chunk emit: %d samples (delta from last intermediate)"),
               FinalDelta);
    }

    // Move the final waveform into OutResult. StreamAudioBuffer is no
    // longer needed after this point — move instead of copy so a
    // several-megabyte TArray<float> doesn't round-trip through memcpy.
    OutResult.AudioSamples   = MoveTemp(StreamAudioBuffer);
    OutResult.SampleRate     = kSampleRate;
    OutResult.TotalElapsedMs = (FPlatformTime::Seconds() - TStart) * 1000.0;

    const double AudioSec =
        (OutResult.SampleRate > 0)
            ? (double)OutResult.AudioSamples.Num() / (double)OutResult.SampleRate
            : 0.0;
    UE_LOG(LogInoAgents, Log,
           TEXT("Chatterbox: Runner: SynthesizeText complete -- %d tokens, %d samples (%.3f s audio), ")
           TEXT("%.1f ms total (encoder=%.0f, embed=%.0f, LM=%.0f, decoder=%.0f, hit_stop=%s)"),
           OutResult.NumGeneratedTokens, OutResult.AudioSamples.Num(), AudioSec,
           OutResult.TotalElapsedMs,
           OutResult.EncoderMs, OutResult.EmbedTotalMs,
           OutResult.LanguageModelMs, OutResult.DecoderMs,
           OutResult.bHitStopToken ? TEXT("yes") : TEXT("no"));

    return true;
}
