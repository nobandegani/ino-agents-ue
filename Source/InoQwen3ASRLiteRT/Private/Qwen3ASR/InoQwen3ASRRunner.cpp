// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoQwen3ASRRunner.h"

#include "InoQwen3ASRConstants.h"
#include "InoQwen3ASRLiteRT.h"

#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
#include "litert/c/litert_common.h"
#include "litert/c/litert_model_types.h"
#endif

namespace
{
#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
    /** Validate one tensor's element type + concrete shape against the spec. */
    static bool ValidateTensorMeta(
        const FInoQwen3ASRLiteRTTensorMeta& Meta,
        const TCHAR* Label,
        LiteRtElementType ExpectedType,
        std::initializer_list<int32> ExpectedDims)
    {
        if (Meta.ElementType != ExpectedType)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("Runner: %s element type mismatch — got %s, expected %s."),
                Label,
                InoQwen3ASRLiteRT::ElementTypeName(Meta.ElementType),
                InoQwen3ASRLiteRT::ElementTypeName(ExpectedType));
            return false;
        }
        if (Meta.Dims.Num() != (int32)ExpectedDims.size())
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("Runner: %s rank mismatch — got %d, expected %d."),
                Label, Meta.Dims.Num(), (int32)ExpectedDims.size());
            return false;
        }
        int32 Idx = 0;
        for (int32 Want : ExpectedDims)
        {
            if (Meta.Dims[Idx] != Want)
            {
                UE_LOG(LogInoQwen3ASRLiteRT, Error,
                    TEXT("Runner: %s dim[%d] mismatch — got %d, expected %d."),
                    Label, Idx, Meta.Dims[Idx], Want);
                return false;
            }
            ++Idx;
        }
        return true;
    }
#endif

    /** Argmax over a contiguous float row of length N. */
    static int32 Argmax(const float* Row, int32 N)
    {
        int32 BestIdx = 0;
        float BestVal = Row[0];
        for (int32 i = 1; i < N; ++i)
        {
            if (Row[i] > BestVal) { BestVal = Row[i]; BestIdx = i; }
        }
        return BestIdx;
    }

    /** True if `Id` is one of the model's two declared EOS tokens. */
    static bool IsEosToken(int32 Id)
    {
        return Id == InoQwen3ASR::kEosTokenIdEndOfText ||
               Id == InoQwen3ASR::kEosTokenIdImEnd;
    }
}

bool FInoQwen3ASRRunner::LoadModel(const FString& TfliteAbsolutePath)
{
    bModelLoaded = false;
    EncodeSigIndex = INDEX_NONE;
    DecodeSigIndex = INDEX_NONE;

#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
    LiteRtEnvironment Env = InoQwen3ASRLiteRT::GetEnvironment();
    if (!Env)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("Runner: LiteRtEnvironment unavailable."));
        return false;
    }

    if (!Model.Load(Env, TfliteAbsolutePath, kLiteRtHwAcceleratorCpu))
    {
        return false;
    }

    EncodeSigIndex = Model.GetSignatureIndex(FName(TEXT("encode")));
    DecodeSigIndex = Model.GetSignatureIndex(FName(TEXT("decode")));
    if (EncodeSigIndex == INDEX_NONE || DecodeSigIndex == INDEX_NONE)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("Runner: model missing required signatures (encode=%d, decode=%d)."),
            EncodeSigIndex, DecodeSigIndex);
        return false;
    }

    // Validate I/O shape contract — better to fail loudly here than to crash
    // mid-Transcribe with a misleading error.
    const FInoQwen3ASRLiteRTSignatureInfo* EncSig = Model.GetSignatureInfo(EncodeSigIndex);
    const FInoQwen3ASRLiteRTSignatureInfo* DecSig = Model.GetSignatureInfo(DecodeSigIndex);
    if (!EncSig || EncSig->Inputs.Num() != 1 || EncSig->Outputs.Num() != 1)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("Runner: encode signature has unexpected I/O count (in=%d out=%d)."),
            EncSig ? EncSig->Inputs.Num() : -1,
            EncSig ? EncSig->Outputs.Num() : -1);
        return false;
    }
    if (!DecSig || DecSig->Inputs.Num() != 3 || DecSig->Outputs.Num() != 1)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("Runner: decode signature has unexpected I/O count (in=%d out=%d)."),
            DecSig ? DecSig->Inputs.Num() : -1,
            DecSig ? DecSig->Outputs.Num() : -1);
        return false;
    }

    using namespace InoQwen3ASR;
    if (!ValidateTensorMeta(EncSig->Inputs[0],  TEXT("encode.in[0] mel"),
            kLiteRtElementTypeFloat32, { 1, kNMels, kMelFrames })) return false;
    if (!ValidateTensorMeta(EncSig->Outputs[0], TEXT("encode.out hidden"),
            kLiteRtElementTypeFloat32, { 1, kEncoderHiddenFrames, kEncoderHiddenDim })) return false;
    if (!ValidateTensorMeta(DecSig->Inputs[0],  TEXT("decode.in[0] hidden"),
            kLiteRtElementTypeFloat32, { 1, kEncoderHiddenFrames, kEncoderHiddenDim })) return false;
    if (!ValidateTensorMeta(DecSig->Inputs[1],  TEXT("decode.in[1] input_ids"),
            kLiteRtElementTypeInt32,   { 1, kDecoderMaxTokens })) return false;
    if (!ValidateTensorMeta(DecSig->Inputs[2],  TEXT("decode.in[2] attn_mask"),
            kLiteRtElementTypeInt32,   { 1, kDecoderMaxTokens })) return false;
    if (!ValidateTensorMeta(DecSig->Outputs[0], TEXT("decode.out logits"),
            kLiteRtElementTypeFloat32, { 1, kDecoderMaxTokens, kVocabSize })) return false;

    bModelLoaded = true;
    UE_LOG(LogInoQwen3ASRLiteRT, Log,
        TEXT("Runner: model loaded and validated. encode=sig[%d], decode=sig[%d]."),
        EncodeSigIndex, DecodeSigIndex);
    return true;
#else
    UE_LOG(LogInoQwen3ASRLiteRT, Warning,
        TEXT("Runner: LiteRT unavailable on this platform."));
    return false;
#endif
}

bool FInoQwen3ASRRunner::LoadTokenizer(const FString& VocabJsonAbsolutePath)
{
    return Tokenizer.LoadFromDisk(VocabJsonAbsolutePath);
}

bool FInoQwen3ASRRunner::Transcribe(
    TArrayView<const float> Audio,
    FString& OutText,
    TArray<int32>* OutTokenIds,
    FInoQwen3ASRTranscribeStats* OutStats)
{
    OutText.Reset();
    if (OutTokenIds) { OutTokenIds->Reset(); }

    if (!IsReady())
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("Runner: Transcribe called before model + tokenizer loaded."));
        return false;
    }

#if !(PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC)
    return false;
#else
    using namespace InoQwen3ASR;

    LiteRtEnvironment Env = InoQwen3ASRLiteRT::GetEnvironment();
    if (!Env) { return false; }

    const double T0Total = FPlatformTime::Seconds();

    // ----------------------------------------------------------------
    // Step 1: log-mel spectrogram
    // ----------------------------------------------------------------
    const double T0Mel = FPlatformTime::Seconds();
    TArray<float> LogMel;
    Mel.Compute(Audio, LogMel);
    const double MelSec = FPlatformTime::Seconds() - T0Mel;
    if (LogMel.Num() != kNMels * kMelFrames)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("Runner: mel computation produced %d elements, expected %d."),
            LogMel.Num(), kNMels * kMelFrames);
        return false;
    }

    // ----------------------------------------------------------------
    // Step 2: encoder pass — single call
    // ----------------------------------------------------------------
    FInoQwen3ASRLiteRTTensor EncIn, EncHidden;
    {
        const int32 EncInDims[]   = { 1, kNMels, kMelFrames };
        const int32 HiddenDims[]  = { 1, kEncoderHiddenFrames, kEncoderHiddenDim };
        if (!EncIn.CreateManagedHost(Env, kLiteRtElementTypeFloat32,
                MakeArrayView(EncInDims, 3))) return false;
        if (!EncHidden.CreateManagedHost(Env, kLiteRtElementTypeFloat32,
                MakeArrayView(HiddenDims, 3))) return false;

        if (float* P = static_cast<float*>(EncIn.LockForWrite()))
        {
            FMemory::Memcpy(P, LogMel.GetData(), LogMel.Num() * sizeof(float));
            EncIn.Unlock();
        }
        else
        {
            return false;
        }
    }

    const double T0Enc = FPlatformTime::Seconds();
    {
        FInoQwen3ASRLiteRTTensor* EncInArr[]  = { &EncIn };
        FInoQwen3ASRLiteRTTensor* EncOutArr[] = { &EncHidden };
        if (!Model.Run(EncodeSigIndex,
                MakeArrayView(EncInArr, 1),
                MakeArrayView(EncOutArr, 1)))
        {
            return false;
        }
    }
    const double EncSec = FPlatformTime::Seconds() - T0Enc;

    // ----------------------------------------------------------------
    // Step 3: AR decode loop
    // ----------------------------------------------------------------
    // Persistent decoder buffers — re-used across iterations. We mutate the
    // input_ids and attention_mask in place between calls; the encoder hidden
    // tensor is read-only after step 2 and the logits output gets overwritten
    // by every call.
    FInoQwen3ASRLiteRTTensor InputIds, AttnMask, Logits;
    {
        const int32 IdDims[]     = { 1, kDecoderMaxTokens };
        const int32 LogitsDims[] = { 1, kDecoderMaxTokens, kVocabSize };
        if (!InputIds.CreateManagedHost(Env, kLiteRtElementTypeInt32,
                MakeArrayView(IdDims, 2))) return false;
        if (!AttnMask.CreateManagedHost(Env, kLiteRtElementTypeInt32,
                MakeArrayView(IdDims, 2))) return false;
        if (!Logits.CreateManagedHost(Env, kLiteRtElementTypeFloat32,
                MakeArrayView(LogitsDims, 3))) return false;

        // Pre-fill input_ids with the chat-template prompt prefix
        // (<|im_start|>system\n...<|im_start|>assistant\n — see
        // GetDecoderPromptPrefix() for the exact 16-token sequence).
        // Positions after the prefix are filled with pad; the mask is 1
        // for prefix positions and 0 for everything else.
        const TArrayView<const int32> Prefix = GetDecoderPromptPrefix();
        const int32 PrefixLen = Prefix.Num();
        check(PrefixLen < kDecoderMaxTokens);

        if (int32* P = static_cast<int32*>(InputIds.LockForWrite()))
        {
            for (int32 i = 0; i < PrefixLen; ++i) { P[i] = Prefix[i]; }
            for (int32 i = PrefixLen; i < kDecoderMaxTokens; ++i) { P[i] = kPadTokenId; }
            InputIds.Unlock();
        }
        else { return false; }

        if (int32* P = static_cast<int32*>(AttnMask.LockForWrite()))
        {
            for (int32 i = 0; i < PrefixLen; ++i) { P[i] = 1; }
            for (int32 i = PrefixLen; i < kDecoderMaxTokens; ++i) { P[i] = 0; }
            AttnMask.Unlock();
        }
        else { return false; }
    }

    const TArrayView<const int32> Prefix = GetDecoderPromptPrefix();
    const int32 PrefixLen = Prefix.Num();

    TArray<int32> Generated;
    Generated.Reserve(kMaxGeneratedTokens - PrefixLen);

    const double T0Dec = FPlatformTime::Seconds();

    // Fill positions PrefixLen, PrefixLen+1, ..., kDecoderMaxTokens-1 with
    // sampled tokens. At iteration k, the model has seen
    // input_ids[0..PrefixLen + k - 1] as real (mask = 1 at those positions);
    // the next-token prediction lives at logits[:, PrefixLen + k - 1, :].
    const int32 MaxSteps = kDecoderMaxTokens - PrefixLen;

    for (int32 Step = 0; Step < MaxSteps; ++Step)
    {
        // Decoder inputs in the signature-declared order:
        //   args_0 = encoder hidden states (cached, never changes)
        //   args_1 = input_ids   (mutated each step at position PrefixLen+Step-1)
        //   args_2 = attention_mask (mask bit set for that position)
        FInoQwen3ASRLiteRTTensor* DecInArr[]  = { &EncHidden, &InputIds, &AttnMask };
        FInoQwen3ASRLiteRTTensor* DecOutArr[] = { &Logits };
        if (!Model.Run(DecodeSigIndex,
                MakeArrayView(DecInArr, 3),
                MakeArrayView(DecOutArr, 1)))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("Runner: decoder Run() failed at step %d."), Step);
            return false;
        }

        // Sample next token from logits at the LAST real position. After
        // pre-fill the last real position is PrefixLen-1; after k-1 sampled
        // tokens it's PrefixLen-1 + k = PrefixLen + Step - 1 ... wait,
        // simpler: at the START of step `Step` we've appended `Step` tokens,
        // so positions 0..PrefixLen+Step-1 are real and we sample from
        // logits[:, PrefixLen+Step-1, :].
        const int32 SamplePos = PrefixLen + Step - 1;
        int32 NextTok = 0;
        {
            const float* L = static_cast<const float*>(Logits.LockForRead());
            if (!L) { return false; }
            const float* Row = L + (SamplePos * kVocabSize);
            NextTok = Argmax(Row, kVocabSize);
            Logits.Unlock();
        }

        if (IsEosToken(NextTok))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Verbose,
                TEXT("Runner: EOS (%d) at step %d, stopping."), NextTok, Step);
            break;
        }

        Generated.Add(NextTok);

        // Append the sampled token at position PrefixLen+Step, mark its
        // mask bit. The next iteration will then sample from
        // logits[:, PrefixLen+Step, :]. If we've just filled the last slot,
        // we stop — there's nowhere left to extend.
        const int32 WritePos = PrefixLen + Step;
        if (WritePos >= kDecoderMaxTokens) { break; }
        {
            int32* P = static_cast<int32*>(InputIds.LockForWrite());
            if (!P) { return false; }
            P[WritePos] = NextTok;
            InputIds.Unlock();
        }
        {
            int32* P = static_cast<int32*>(AttnMask.LockForWrite());
            if (!P) { return false; }
            P[WritePos] = 1;
            AttnMask.Unlock();
        }
    }

    const double DecSec = FPlatformTime::Seconds() - T0Dec;

    // ----------------------------------------------------------------
    // Step 4: strip the language-tag prefix, then detokenize
    // ----------------------------------------------------------------
    // Qwen3-ASR's assistant turn always begins with the auto-detected
    // language tag in the form:
    //     "language" + " <LangName>" + <special separator> + <transcription>
    //
    // Where:
    //   - "language" is the plain-text BPE token (id 11528)
    //   - " English" / " Chinese" / etc. is one (or sometimes two) plain
    //     text tokens that name the detected language
    //   - the separator is a single special token in the >= kPadTokenId
    //     range (id 151704 for "English" auto-detection, possibly a
    //     different special id per language)
    //   - everything after the separator is the actual transcription
    //
    // Strip the prefix when we recognise the marker; keep all tokens
    // otherwise so we never silently drop real text. The raw IDs are
    // always handed back via OutTokenIds for debugging.
    int32 TextStart = 0;
    if (Generated.Num() >= 3 && Generated[0] == 11528 /* "language" */)
    {
        for (int32 i = 1; i < Generated.Num(); ++i)
        {
            if (Generated[i] >= kPadTokenId && !IsEosToken(Generated[i]))
            {
                TextStart = i + 1;
                break;
            }
        }
    }

    const TArrayView<const int32> TextSlice =
        TArrayView<const int32>(Generated.GetData() + TextStart,
                                Generated.Num() - TextStart);
    OutText = Tokenizer.Decode(TextSlice);
    if (OutTokenIds) { *OutTokenIds = Generated; }

    if (OutStats)
    {
        OutStats->NumGeneratedTokens = Generated.Num();
        OutStats->MelSeconds         = MelSec;
        OutStats->EncodeSeconds      = EncSec;
        OutStats->DecodeSeconds      = DecSec;
        OutStats->TotalSeconds       = FPlatformTime::Seconds() - T0Total;
    }

    return true;
#endif
}
