// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTTSDecoderSession.h"

#include "InoNeuTTSCommon.h"  // BackendToLiteRtAcceleratorBit

#include "InoAgentsLog.h"

#include "litert/c/litert_common.h"
#include "litert/c/litert_compiled_model.h"
#include "litert/c/litert_environment.h"
#include "litert/c/litert_layout.h"
#include "litert/c/litert_model.h"
#include "litert/c/litert_model_types.h"
#include "litert/c/litert_options.h"
#include "litert/c/litert_tensor_buffer.h"
#include "litert/c/litert_tensor_buffer_requirements.h"
#include "litert/c/litert_tensor_buffer_types.h"

namespace
{
    static bool CheckStatusLog(LiteRtStatus Status, const TCHAR* What, FString& OutError)
    {
        if (Status == kLiteRtStatusOk) return true;
        OutError = FString::Printf(TEXT("LiteRT call '%s' failed: status=%d"),
                                   What, static_cast<int32>(Status));
        UE_LOG(LogInoAgents, Error, TEXT("[NeuTTS][Decoder] %s"), *OutError);
        return false;
    }
}

TUniquePtr<FInoNeuTTSDecoderSession> FInoNeuTTSDecoderSession::Create(
    const FString& ModelPath, EInoNeuTTSBackend Backend, FString& OutError)
{
    TUniquePtr<FInoNeuTTSDecoderSession> Inst(new FInoNeuTTSDecoderSession());
    if (!Inst->Initialize(ModelPath, Backend, OutError)) return nullptr;
    return Inst;
}

FInoNeuTTSDecoderSession::~FInoNeuTTSDecoderSession()
{
    // Destroy cached tensor buffers BEFORE the model/environment they
    // depend on. Tensor buffers keep an opaque reference to the env
    // internally.
    if (Cache.InBuf)  { LiteRtDestroyTensorBuffer(Cache.InBuf);  Cache.InBuf  = nullptr; }
    if (Cache.OutBuf) { LiteRtDestroyTensorBuffer(Cache.OutBuf); Cache.OutBuf = nullptr; }

    if (CompiledModel) LiteRtDestroyCompiledModel(CompiledModel);
    if (Options)       LiteRtDestroyOptions(Options);
    if (Model)         LiteRtDestroyModel(Model);
    if (Environment)   LiteRtDestroyEnvironment(Environment);
}

bool FInoNeuTTSDecoderSession::Initialize(
    const FString& ModelPath, EInoNeuTTSBackend Backend, FString& OutError)
{
    if (!CheckStatusLog(LiteRtCreateEnvironment(0, nullptr, &Environment),
                        TEXT("CreateEnvironment"), OutError)) return false;

    const FTCHARToUTF8 PathUtf8(*ModelPath);
    if (!CheckStatusLog(LiteRtCreateModelFromFile(PathUtf8.Get(), &Model),
                        TEXT("CreateModelFromFile"), OutError)) return false;

    // Enumerate signatures, parse the `f<N>` bucket size from each name.
    LiteRtParamIndex NumSigs = 0;
    if (!CheckStatusLog(LiteRtGetNumModelSignatures(Model, &NumSigs),
                        TEXT("GetNumModelSignatures"), OutError)) return false;
    for (LiteRtParamIndex i = 0; i < NumSigs; ++i)
    {
        LiteRtSignature Sig = nullptr;
        LiteRtGetModelSignature(Model, i, &Sig);
        const char* Key = nullptr;
        LiteRtGetSignatureKey(Sig, &Key);
        const FString KeyStr = Key ? ANSI_TO_TCHAR(Key) : FString();
        if (KeyStr.Len() > 1 && KeyStr[0] == TEXT('f'))
        {
            const int32 N = FCString::Atoi(*KeyStr.RightChop(1));
            if (N > 0)
            {
                Signatures.Add({ static_cast<int32>(i), N });
            }
        }
    }
    Signatures.Sort([](const FSignatureInfo& A, const FSignatureInfo& B)
                    { return A.BucketFrames < B.BucketFrames; });
    if (Signatures.Num() == 0)
    {
        OutError = TEXT("No 'f<N>' signatures found in the decoder model — ")
                   TEXT("file may be from a different NeuCodec export.");
        UE_LOG(LogInoAgents, Error, TEXT("[NeuTTS][Decoder] %s"), *OutError);
        return false;
    }
    MaxBucketFrames = Signatures.Last().BucketFrames;
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][Decoder] %d signatures, buckets up to f%d"),
        Signatures.Num(), MaxBucketFrames);

    // Compile with the caller-selected accelerator.
    if (!CheckStatusLog(LiteRtCreateOptions(&Options),
                        TEXT("CreateOptions"), OutError)) return false;
    const int32 AcceleratorBit = InoNeuTTSNative::BackendToLiteRtAcceleratorBit(Backend);
    CheckStatusLog(LiteRtSetOptionsHardwareAccelerators(
                       Options,
                       static_cast<LiteRtHwAcceleratorSet>(AcceleratorBit)),
                   TEXT("SetOptionsHardwareAccelerators"), OutError);
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][Decoder] compiling with accelerator bit 0x%x"),
        AcceleratorBit);
    if (!CheckStatusLog(LiteRtCreateCompiledModel(Environment, Model, Options, &CompiledModel),
                        TEXT("CreateCompiledModel"), OutError)) return false;

    return true;
}

bool FInoNeuTTSDecoderSession::Decode(
    TArrayView<const int32> Codes, TArray<float>& OutWaveform, FString& OutError)
{
    OutWaveform.Reset();
    if (Codes.Num() <= 0)
    {
        OutError = TEXT("Codes array is empty");
        return false;
    }

    // Pick the smallest bucket that fits.
    const FSignatureInfo* Picked = nullptr;
    for (const FSignatureInfo& S : Signatures)
    {
        if (S.BucketFrames >= Codes.Num()) { Picked = &S; break; }
    }
    if (!Picked)
    {
        OutError = FString::Printf(TEXT("Codes.Num()=%d exceeds max decoder bucket f%d"),
                                   Codes.Num(), MaxBucketFrames);
        UE_LOG(LogInoAgents, Error, TEXT("[NeuTTS][Decoder] %s"), *OutError);
        return false;
    }
    const LiteRtParamIndex SigIdx = static_cast<LiteRtParamIndex>(Picked->SignatureIndex);

    // ----- Tensor-buffer cache: reuse if the bucket matches the cached one.
    if (Cache.SignatureIndex != Picked->SignatureIndex)
    {
        // Different bucket — drop the cached buffers, recreate.
        if (Cache.InBuf)  { LiteRtDestroyTensorBuffer(Cache.InBuf);  Cache.InBuf  = nullptr; }
        if (Cache.OutBuf) { LiteRtDestroyTensorBuffer(Cache.OutBuf); Cache.OutBuf = nullptr; }
        Cache.SignatureIndex = -1;

        // Tensor types (input is `codes:int64[1,1,F]`, output is `audio:float32[1,1,(F-1)*480]`).
        LiteRtSignature Sig = nullptr;
        LiteRtGetModelSignature(Model, SigIdx, &Sig);
        LiteRtTensor InT = nullptr;  LiteRtGetSignatureInputTensorByIndex(Sig, 0, &InT);
        LiteRtTensor OutT = nullptr; LiteRtGetSignatureOutputTensorByIndex(Sig, 0, &OutT);
        LiteRtRankedTensorType InType{}, OutType{};
        LiteRtGetRankedTensorType(InT, &InType);
        LiteRtGetRankedTensorType(OutT, &OutType);

        LiteRtTensorBufferRequirements InReqs = nullptr;
        if (!CheckStatusLog(LiteRtGetCompiledModelInputBufferRequirements(CompiledModel, SigIdx, 0, &InReqs),
                            TEXT("GetInputBufferRequirements"), OutError)) return false;
        LiteRtTensorBufferRequirements OutReqs = nullptr;
        if (!CheckStatusLog(LiteRtGetCompiledModelOutputBufferRequirements(CompiledModel, SigIdx, 0, &OutReqs),
                            TEXT("GetOutputBufferRequirements"), OutError)) return false;

        if (!CheckStatusLog(LiteRtCreateManagedTensorBufferFromRequirements(Environment, &InType, InReqs, &Cache.InBuf),
                            TEXT("CreateInputBuffer"), OutError)) return false;
        if (!CheckStatusLog(LiteRtCreateManagedTensorBufferFromRequirements(Environment, &OutType, OutReqs, &Cache.OutBuf),
                            TEXT("CreateOutputBuffer"), OutError))
        {
            LiteRtDestroyTensorBuffer(Cache.InBuf);
            Cache.InBuf = nullptr;
            return false;
        }
        Cache.SignatureIndex = Picked->SignatureIndex;
    }

    LiteRtTensorBuffer InBuf  = Cache.InBuf;
    LiteRtTensorBuffer OutBuf = Cache.OutBuf;

    bool bOK = true;

    // Write input (int64 codes, zero-padded to bucket size).
    {
        size_t InPacked = 0;
        LiteRtGetTensorBufferPackedSize(InBuf, &InPacked);
        const size_t NumInElems = InPacked / sizeof(int64_t);
        void* InAddr = nullptr;
        bOK = CheckStatusLog(LiteRtLockTensorBuffer(InBuf, &InAddr, kLiteRtTensorBufferLockModeWrite),
                             TEXT("LockInput"), OutError);
        if (bOK)
        {
            int64_t* Dst = static_cast<int64_t*>(InAddr);
            for (int32 i = 0; i < Codes.Num(); ++i) { Dst[i] = static_cast<int64_t>(Codes[i]); }
            for (size_t i = Codes.Num(); i < NumInElems; ++i) { Dst[i] = 0; }
            LiteRtUnlockTensorBuffer(InBuf);
        }
    }

    // Run.
    if (bOK)
    {
        LiteRtTensorBuffer Ins[1]  = { InBuf };
        LiteRtTensorBuffer Outs[1] = { OutBuf };
        bOK = CheckStatusLog(LiteRtRunCompiledModel(CompiledModel, SigIdx, 1, Ins, 1, Outs),
                             TEXT("RunCompiledModel"), OutError);
    }

    // Read output (float32 audio, trim trailing zero-padded samples).
    if (bOK)
    {
        size_t OutPacked = 0;
        LiteRtGetTensorBufferPackedSize(OutBuf, &OutPacked);
        const size_t NumOutElems = OutPacked / sizeof(float);

        void* OutAddr = nullptr;
        bOK = CheckStatusLog(LiteRtLockTensorBuffer(OutBuf, &OutAddr, kLiteRtTensorBufferLockModeRead),
                             TEXT("LockOutput"), OutError);
        if (bOK)
        {
            const float* Src = static_cast<const float*>(OutAddr);
            // Per the conversion script, the valid audio length for an
            // F-frame input is (F-1) * hop = (Codes.Num()-1) * 480
            // — the iSTFT edge trim consumes the last frame.
            const int32 ValidSamples = (Codes.Num() - 1) * 480;
            const int32 CopyCount = FMath::Clamp(ValidSamples, 0,
                                                 static_cast<int32>(NumOutElems));
            OutWaveform.SetNumUninitialized(CopyCount);
            if (CopyCount > 0)
            {
                FMemory::Memcpy(OutWaveform.GetData(), Src, CopyCount * sizeof(float));
            }
            LiteRtUnlockTensorBuffer(OutBuf);
        }
    }

    // Do NOT destroy InBuf / OutBuf — they live on Cache for the next call.
    return bOK;
}

bool FInoNeuTTSDecoderSession::Warmup(FString& OutError)
{
    if (Signatures.Num() == 0)
    {
        OutError = TEXT("No signatures to warm up");
        return false;
    }
    // Smallest bucket, all-zero FSQ codes — just exercises the kernel
    // path. Output (silence-shaped noise) is discarded.
    TArray<int32> Dummy;
    Dummy.Init(0, Signatures[0].BucketFrames);
    TArray<float> Throwaway;
    return Decode(Dummy, Throwaway, OutError);
}
