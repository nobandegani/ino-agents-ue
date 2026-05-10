// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Qwen3ASR/InoQwen3ASRLiteRTEnv.h"
#include "Qwen3ASR/InoQwen3ASRLiteRTModel.h"
#include "Qwen3ASR/InoQwen3ASRLiteRTTensor.h"
#include "InoQwen3ASRLiteRT.h"

#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
#include "litert/c/litert_common.h"
#include "litert/c/litert_model_types.h"
#endif

namespace
{
    /** Resolve InoAgents/Qwen3ASR/LiteRT/models/<file>. */
    FString ResolveModelPath(const TCHAR* FileName)
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (!Plugin.IsValid()) { return FString(); }
        return FPaths::Combine(
            Plugin->GetBaseDir(),
            TEXT("Qwen3ASR"), TEXT("LiteRT"), TEXT("models"),
            FileName);
    }

    /**
     * Phase 1 smoke test for InoQwen3ASRLiteRT.
     *
     * Loads qwen3_asr_0.6b_5s_i8.tflite, enumerates every signature with
     * its input/output names + dtypes + shapes, and runs one forward pass
     * with a zero-filled input (silent audio) using the shapes declared
     * in the signature.
     *
     * The signature dump tells us how the encoder + decoder + KV cache are
     * structured inside the single .tflite — which determines how the
     * Phase 2 inference orchestrator needs to be wired:
     *   - One signature (audio + prev tokens → next token logits): simple
     *     AR loop on the caller side
     *   - Two signatures (encode → encoder hidden states, decode → next
     *     token): cleaner separation
     *   - Multi-signature with externally-bound KV (prefill_N + decode):
     *     mirror the Gemma/Llama pattern with LiteRtAddExternalTensorBinding
     */
    void RunLoadTest(const TArray<FString>& /*Args*/)
    {
#if !(PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC)
        UE_LOG(LogInoQwen3ASRLiteRT, Warning,
            TEXT("Ino.Qwen3ASRLiteRT.LoadTest: not supported on this platform."));
#else
        UE_LOG(LogInoQwen3ASRLiteRT, Log, TEXT("=== Ino.Qwen3ASRLiteRT.LoadTest ==="));

        LiteRtEnvironment Env = InoQwen3ASRLiteRT::GetEnvironment();
        if (!Env)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("LoadTest: failed to create LiteRtEnvironment."));
            return;
        }

        const FString Path = ResolveModelPath(TEXT("qwen3_asr_0.6b_5s_i8.tflite"));
        if (Path.IsEmpty() || !FPaths::FileExists(Path))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("LoadTest: model file missing. Expected at: %s"), *Path);
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("Download from: https://huggingface.co/litert-community/Qwen3-ASR-0.6B/resolve/main/qwen3_asr_0.6b_5s_i8.tflite"));
            return;
        }

        const double T0Load = FPlatformTime::Seconds();
        TUniquePtr<FInoQwen3ASRLiteRTModel> Model = MakeUnique<FInoQwen3ASRLiteRTModel>();
        if (!Model->Load(Env, Path, kLiteRtHwAcceleratorCpu))
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error, TEXT("LoadTest: model load FAILED."));
            return;
        }
        const double LoadElapsed = FPlatformTime::Seconds() - T0Load;
        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("LoadTest: model loaded in %.2fs (%d signatures, fully_accelerated=%s)."),
            LoadElapsed, Model->NumSignatures(),
            Model->IsFullyAccelerated() ? TEXT("yes") : TEXT("no"));
        Model->LogSignatures();

        // Allocate input + output for the FIRST signature using declared shapes.
        // For static-shape litert-community models this should be unambiguous.
        const FInoQwen3ASRLiteRTSignatureInfo* Sig = Model->GetSignatureInfo(0);
        if (!Sig)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("LoadTest: no signature 0."));
            return;
        }

        // Bail if any input has dynamic dimensions — we'd need to know the
        // model's expected concrete shape, which we don't a priori.
        for (int32 i = 0; i < Sig->Inputs.Num(); ++i)
        {
            for (int32 d : Sig->Inputs[i].Dims)
            {
                if (d <= 0)
                {
                    UE_LOG(LogInoQwen3ASRLiteRT, Warning,
                        TEXT("LoadTest: input %d (%s) has dynamic dim — skipping forward run. "
                             "Add resize logic if this surfaces."),
                        i, *Sig->Inputs[i].Name.ToString());
                    return;
                }
            }
        }

        // Allocate input buffers and fill with zeros.
        TArray<TUniquePtr<FInoQwen3ASRLiteRTTensor>> InputBufs;
        InputBufs.Reserve(Sig->Inputs.Num());
        TArray<FInoQwen3ASRLiteRTTensor*> InputArray;
        InputArray.Reserve(Sig->Inputs.Num());
        for (int32 i = 0; i < Sig->Inputs.Num(); ++i)
        {
            const FInoQwen3ASRLiteRTTensorMeta& M = Sig->Inputs[i];
            TUniquePtr<FInoQwen3ASRLiteRTTensor> Buf = MakeUnique<FInoQwen3ASRLiteRTTensor>();
            if (!Buf->CreateManagedHost(Env, M.ElementType, MakeArrayView(M.Dims.GetData(), M.Dims.Num())))
            {
                UE_LOG(LogInoQwen3ASRLiteRT, Error,
                    TEXT("LoadTest: failed to allocate input buffer %d (%s)."),
                    i, *M.Name.ToString());
                return;
            }
            // Zero-fill so we know the input is well-defined (silent audio).
            if (void* Ptr = Buf->LockForWrite())
            {
                FMemory::Memzero(Ptr, Buf->PackedBytes());
                Buf->Unlock();
            }
            InputArray.Add(Buf.Get());
            InputBufs.Add(MoveTemp(Buf));
        }

        // Allocate output buffers using declared shapes.
        TArray<TUniquePtr<FInoQwen3ASRLiteRTTensor>> OutputBufs;
        OutputBufs.Reserve(Sig->Outputs.Num());
        TArray<FInoQwen3ASRLiteRTTensor*> OutputArray;
        OutputArray.Reserve(Sig->Outputs.Num());
        for (int32 i = 0; i < Sig->Outputs.Num(); ++i)
        {
            const FInoQwen3ASRLiteRTTensorMeta& M = Sig->Outputs[i];
            // If output has dynamic dims we can't pre-allocate — bail loudly.
            for (int32 d : M.Dims)
            {
                if (d <= 0)
                {
                    UE_LOG(LogInoQwen3ASRLiteRT, Warning,
                        TEXT("LoadTest: output %d (%s) has dynamic dim — skipping forward run."),
                        i, *M.Name.ToString());
                    return;
                }
            }
            TUniquePtr<FInoQwen3ASRLiteRTTensor> Buf = MakeUnique<FInoQwen3ASRLiteRTTensor>();
            if (!Buf->CreateManagedHost(Env, M.ElementType, MakeArrayView(M.Dims.GetData(), M.Dims.Num())))
            {
                UE_LOG(LogInoQwen3ASRLiteRT, Error,
                    TEXT("LoadTest: failed to allocate output buffer %d (%s)."),
                    i, *M.Name.ToString());
                return;
            }
            OutputArray.Add(Buf.Get());
            OutputBufs.Add(MoveTemp(Buf));
        }

        // Run.
        const double T0Run = FPlatformTime::Seconds();
        const bool bRan = Model->Run(0, InputArray, OutputArray);
        const double RunElapsed = FPlatformTime::Seconds() - T0Run;
        if (!bRan)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error, TEXT("LoadTest: Run() FAILED."));
            return;
        }
        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("LoadTest: Run() returned in %.4fs"), RunElapsed);

        // Inspect every output tensor for sanity.
        for (int32 i = 0; i < OutputBufs.Num(); ++i)
        {
            FInoQwen3ASRLiteRTTensor* Buf = OutputBufs[i].Get();
            const FInoQwen3ASRLiteRTTensorMeta& M = Sig->Outputs[i];
            const size_t Bytes = Buf->PackedBytes();
            const size_t ElemBytes = InoQwen3ASRLiteRT::ElementByteSize(M.ElementType);
            const size_t NumElems = ElemBytes ? (Bytes / ElemBytes) : 0;

            const void* RawPtr = Buf->LockForRead();
            if (!RawPtr || NumElems == 0)
            {
                UE_LOG(LogInoQwen3ASRLiteRT, Error,
                    TEXT("    output[%d] %s: lock failed or empty"), i, *M.Name.ToString());
                if (RawPtr) Buf->Unlock();
                continue;
            }

            int32 NonZeroCount = 0;
            int32 NanCount = 0;
            double MeanAbs = 0.0;
            double MinV = 0.0;
            double MaxV = 0.0;

            // Treat as fp32 if it's the float type, else as int values.
            if (M.ElementType == kLiteRtElementTypeFloat32)
            {
                const float* P = static_cast<const float*>(RawPtr);
                MinV = MaxV = P[0];
                for (size_t k = 0; k < NumElems; ++k)
                {
                    const float V = P[k];
                    if (FMath::IsNaN(V)) { ++NanCount; continue; }
                    if (V != 0.0f) { ++NonZeroCount; }
                    MeanAbs += FMath::Abs(static_cast<double>(V));
                    if (V < MinV) MinV = V;
                    if (V > MaxV) MaxV = V;
                }
                MeanAbs /= NumElems;
            }
            else if (M.ElementType == kLiteRtElementTypeInt32)
            {
                const int32* P = static_cast<const int32*>(RawPtr);
                MinV = MaxV = static_cast<double>(P[0]);
                for (size_t k = 0; k < NumElems; ++k)
                {
                    const int32 V = P[k];
                    if (V != 0) { ++NonZeroCount; }
                    const double Vd = static_cast<double>(V);
                    MeanAbs += FMath::Abs(Vd);
                    if (Vd < MinV) MinV = Vd;
                    if (Vd > MaxV) MaxV = Vd;
                }
                MeanAbs /= NumElems;
            }
            else if (M.ElementType == kLiteRtElementTypeInt64)
            {
                const int64* P = static_cast<const int64*>(RawPtr);
                MinV = MaxV = static_cast<double>(P[0]);
                for (size_t k = 0; k < NumElems; ++k)
                {
                    const int64 V = P[k];
                    if (V != 0) { ++NonZeroCount; }
                    const double Vd = static_cast<double>(V);
                    MeanAbs += FMath::Abs(Vd);
                    if (Vd < MinV) MinV = Vd;
                    if (Vd > MaxV) MaxV = Vd;
                }
                MeanAbs /= NumElems;
            }
            else
            {
                UE_LOG(LogInoQwen3ASRLiteRT, Log,
                    TEXT("    output[%d] %s: dtype %s — skipping numeric scan."),
                    i, *M.Name.ToString(),
                    InoQwen3ASRLiteRT::ElementTypeName(M.ElementType));
                Buf->Unlock();
                continue;
            }

            UE_LOG(LogInoQwen3ASRLiteRT, Log,
                TEXT("    output[%d] %s: elems=%llu non_zero=%d nan=%d mean_abs=%.4e range=[%.4e, %.4e]"),
                i, *M.Name.ToString(),
                static_cast<uint64>(NumElems), NonZeroCount, NanCount, MeanAbs, MinV, MaxV);
            Buf->Unlock();
        }

        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("LoadTest: complete. Inspect the output stats above — if mean_abs "
                 "is reasonable (>~1e-4) and nan_count=0, the LiteRT path is fully working."));
#endif
    }

    static FAutoConsoleCommand GLoadTestCmd(
        TEXT("Ino.Qwen3ASRLiteRT.LoadTest"),
        TEXT("Phase 1 of the Qwen3-ASR-0.6B integration: load the .tflite, dump "
             "every signature with I/O shapes, run a forward pass with zero-filled "
             "inputs to confirm the runtime path works. The signature dump tells us "
             "how the encoder/decoder/KV-cache are structured inside the model."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunLoadTest));
}
