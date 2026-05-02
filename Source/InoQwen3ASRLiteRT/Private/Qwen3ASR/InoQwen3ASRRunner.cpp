// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoQwen3ASRRunner.h"

#include "InoQwen3ASRConstants.h"
#include "InoQwen3ASRLiteRT.h"

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
#include "litert/c/litert_common.h"
#include "litert/c/litert_model_types.h"
#endif

namespace
{
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

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
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

#if !(PLATFORM_WINDOWS || PLATFORM_ANDROID)
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

        // Fill input_ids with pad and mask with 0. Decoder starts with no
        // prefix tokens — its first sampled token comes from logits[:, 0, :]
        // where the model conditions purely on encoder cross-attention.
        if (int32* P = static_cast<int32*>(InputIds.LockForWrite()))
        {
            for (int32 i = 0; i < kDecoderMaxTokens; ++i) { P[i] = kPadTokenId; }
            InputIds.Unlock();
        }
        else { return false; }

        if (int32* P = static_cast<int32*>(AttnMask.LockForWrite()))
        {
            FMemory::Memzero(P, kDecoderMaxTokens * sizeof(int32));
            AttnMask.Unlock();
        }
        else { return false; }
    }

    TArray<int32> Generated;
    Generated.Reserve(kMaxGeneratedTokens);

    const double T0Dec = FPlatformTime::Seconds();

    for (int32 Step = 0; Step < kMaxGeneratedTokens; ++Step)
    {
        // Decoder inputs in the order declared by the signature:
        //   args_0 = encoder hidden states (cached)
        //   args_1 = input_ids
        //   args_2 = attention_mask
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

        // Sample next token from logits[:, Step, :]. With an all-zero mask
        // and no prefix, position 0 holds the first emission; subsequent
        // calls put the next prediction at position `Step`.
        int32 NextTok = 0;
        {
            const float* L = static_cast<const float*>(Logits.LockForRead());
            if (!L) { return false; }
            const float* Row = L + (Step * kVocabSize);
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

        // Write the new token into position `Step` of input_ids and mark its
        // mask bit. The next iteration's logits[:, Step+1, :] then predicts
        // the token after this one.
        const int32 WritePos = Step;
        if (WritePos >= kDecoderMaxTokens - 1)
        {
            // We've filled the buffer; can't condition any further on this
            // token in a future step. Stop here rather than emit a token we
            // can't extend from.
            break;
        }
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
    // Step 4: detokenize
    // ----------------------------------------------------------------
    OutText = Tokenizer.Decode(Generated);
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
