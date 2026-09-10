// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// =====================================================================
// Phase 0a spike — bare LiteRT C API.
// =====================================================================
//
// Proves that:
//
//   1. The bare LiteRT C API (`litert/c/litert_*.h`) symbols link against
//      libLiteRt.dll which InoLiteRT staged in its PreLoadingScreen
//      StartupModule. No current consumer in this repo touches these
//      headers (InoLiteRtLm uses only `litert/lm/engine.h`), so this is
//      the first stress test of that half of InoLiteRT's surface.
//
//   2. The user's converted NeuCodec decoder .tflite (output of
//      `Plugins/InoLiteRT/Convert/NeuCodec/scripts/convert_to_tflite.py`)
//      loads, exposes one signature per frame-count bucket (`f50`, `f100`,
//      `f200`, ...), and produces float32 audio of shape `[1, 1, (F-1)*480]`
//      when fed int64 codes of shape `[1, 1, F]`.
//
// If this command fails or links wrong, the fix is in InoLiteRT (binary
// staging / accelerator wiring / DLL exports), not in InoNeuTTS. Run this
// before building the real `FInoNeuTTSDecoderSession`.
//
// Usage:
//   Ino.NeuTTS.DecoderProbeTest <abs path to neucodec_decoder_*.tflite>
//
// Example:
//   Ino.NeuTTS.DecoderProbeTest <YourProject>/Plugins/InoLiteRT/Convert/NeuCodec/output/neucodec_decoder_q8.tflite

#include "InoAgentsLog.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Math/UnrealMathUtility.h"

#include "litert/c/litert_common.h"
#include "litert/c/litert_environment.h"
#include "litert/c/litert_compiled_model.h"
#include "litert/c/litert_layout.h"
#include "litert/c/litert_model.h"
#include "litert/c/litert_model_types.h"
#include "litert/c/litert_options.h"
#include "litert/c/litert_tensor_buffer.h"
#include "litert/c/litert_tensor_buffer_requirements.h"
#include "litert/c/litert_tensor_buffer_types.h"

namespace
{

// ---------------------------------------------------------------------
// Tiny error-check helper. The C API returns LiteRtStatus by value; we
// log + early-return through the OutError pointer when non-OK.
// ---------------------------------------------------------------------
static bool CheckStatus(LiteRtStatus Status, const TCHAR* What)
{
    if (Status == kLiteRtStatusOk)
    {
        return true;
    }
    // `LiteRtGetStatusString` is declared in `litert_common.h` but is NOT
    // exported from libLiteRt.dll in the InoLiteRT-staged build (verified
    // 2026-05-11 by linker error). Log the integer code only — the
    // `kLiteRtStatus*` enum values are stable and grep-able in the
    // header. If a future LiteRT bump exports the symbol, swap in the
    // string version.
    UE_LOG(LogInoAgents, Error,
        TEXT("[NeuTTS][DecoderProbe] %s failed: status=%d"),
        What, static_cast<int32>(Status));
    return false;
}

// ---------------------------------------------------------------------
// Stringify LiteRT primitive element types for logs.
// ---------------------------------------------------------------------
static const TCHAR* ElementTypeName(LiteRtElementType T)
{
    switch (T)
    {
        case kLiteRtElementTypeFloat32: return TEXT("float32");
        case kLiteRtElementTypeFloat16: return TEXT("float16");
        case kLiteRtElementTypeInt8:    return TEXT("int8");
        case kLiteRtElementTypeInt16:   return TEXT("int16");
        case kLiteRtElementTypeInt32:   return TEXT("int32");
        case kLiteRtElementTypeInt64:   return TEXT("int64");
        case kLiteRtElementTypeUInt8:   return TEXT("uint8");
        case kLiteRtElementTypeBool:    return TEXT("bool");
        default: return TEXT("<other>");
    }
}

static FString FormatShape(const LiteRtRankedTensorType& T)
{
    FString Result = TEXT("[");
    for (uint32 i = 0; i < T.layout.rank; ++i)
    {
        if (i > 0) Result += TEXT(", ");
        Result += FString::Printf(TEXT("%d"), T.layout.dimensions[i]);
    }
    Result += TEXT("]");
    return Result;
}

// ---------------------------------------------------------------------
// Walk the model's signatures, log each one's I/O shape + dtype, and
// pick the smallest bucket (parsed from `f<N>` signature names).
// Returns the chosen signature index, or 0 if no `f<N>` style names
// were found.
// ---------------------------------------------------------------------
static LiteRtParamIndex EnumerateAndPickSmallestSignature(LiteRtModel Model,
                                                          LiteRtParamIndex NumSigs)
{
    LiteRtParamIndex BestIdx = 0;
    int32 BestN = INT32_MAX;
    bool bAnyParsed = false;

    for (LiteRtParamIndex i = 0; i < NumSigs; ++i)
    {
        LiteRtSignature Sig = nullptr;
        if (!CheckStatus(LiteRtGetModelSignature(Model, i, &Sig),
                         TEXT("LiteRtGetModelSignature")))
        {
            continue;
        }

        const char* Key = nullptr;
        LiteRtGetSignatureKey(Sig, &Key);
        const FString KeyStr = Key ? ANSI_TO_TCHAR(Key) : FString();

        // Log input metadata.
        LiteRtParamIndex NumIn = 0;
        LiteRtGetNumSignatureInputs(Sig, &NumIn);
        FString InDesc;
        for (LiteRtParamIndex j = 0; j < NumIn; ++j)
        {
            const char* Name = nullptr;
            LiteRtGetSignatureInputName(Sig, j, &Name);
            LiteRtTensor T = nullptr;
            LiteRtGetSignatureInputTensorByIndex(Sig, j, &T);
            LiteRtRankedTensorType RT = {};
            LiteRtGetRankedTensorType(T, &RT);
            if (j > 0) InDesc += TEXT(", ");
            InDesc += FString::Printf(TEXT("%s:%s%s"),
                Name ? ANSI_TO_TCHAR(Name) : TEXT("?"),
                ElementTypeName(RT.element_type),
                *FormatShape(RT));
        }

        // Log output metadata.
        LiteRtParamIndex NumOut = 0;
        LiteRtGetNumSignatureOutputs(Sig, &NumOut);
        FString OutDesc;
        for (LiteRtParamIndex j = 0; j < NumOut; ++j)
        {
            const char* Name = nullptr;
            LiteRtGetSignatureOutputName(Sig, j, &Name);
            LiteRtTensor T = nullptr;
            LiteRtGetSignatureOutputTensorByIndex(Sig, j, &T);
            LiteRtRankedTensorType RT = {};
            LiteRtGetRankedTensorType(T, &RT);
            if (j > 0) OutDesc += TEXT(", ");
            OutDesc += FString::Printf(TEXT("%s:%s%s"),
                Name ? ANSI_TO_TCHAR(Name) : TEXT("?"),
                ElementTypeName(RT.element_type),
                *FormatShape(RT));
        }

        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][DecoderProbe] sig[%llu] '%s'  in={%s}  out={%s}"),
            static_cast<uint64>(i), *KeyStr, *InDesc, *OutDesc);

        // Parse the `f<N>` bucket size.
        if (KeyStr.Len() > 1 && KeyStr[0] == TEXT('f'))
        {
            const FString Suffix = KeyStr.RightChop(1);
            const int32 N = FCString::Atoi(*Suffix);
            if (N > 0)
            {
                bAnyParsed = true;
                if (N < BestN)
                {
                    BestN = N;
                    BestIdx = i;
                }
            }
        }
    }

    if (bAnyParsed)
    {
        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][DecoderProbe] picked signature index %llu (bucket f%d)"),
            static_cast<uint64>(BestIdx), BestN);
    }
    else
    {
        UE_LOG(LogInoAgents, Warning,
            TEXT("[NeuTTS][DecoderProbe] no 'f<N>' signatures parsed — defaulting to index 0. ")
            TEXT("This may indicate a different-than-expected NeuCodec export."));
    }

    return BestIdx;
}

// ---------------------------------------------------------------------
// Main entry — wires everything together.
// ---------------------------------------------------------------------
static void RunDecoderProbeTest(const TArray<FString>& Args)
{
    if (Args.Num() < 1)
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][DecoderProbe] usage: Ino.NeuTTS.DecoderProbeTest <abs path to .tflite>"));
        return;
    }

    const FString ModelPath = Args[0];
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][DecoderProbe] === BEGIN === model=%s"), *ModelPath);

    // -----------------------------------------------------------------
    // 1. Create environment (CPU only — keep the spike minimal; the
    //    real FInoNeuTTSDecoderSession will let callers pick GPU/NPU).
    // -----------------------------------------------------------------
    LiteRtEnvironment Env = nullptr;
    if (!CheckStatus(LiteRtCreateEnvironment(0, nullptr, &Env),
                     TEXT("LiteRtCreateEnvironment"))) return;

    // -----------------------------------------------------------------
    // 2. Load the model from the user's path.
    // -----------------------------------------------------------------
    const FTCHARToUTF8 PathUtf8(*ModelPath);
    LiteRtModel Model = nullptr;
    const double LoadStart = FPlatformTime::Seconds();
    if (!CheckStatus(LiteRtCreateModelFromFile(PathUtf8.Get(), &Model),
                     TEXT("LiteRtCreateModelFromFile")))
    {
        LiteRtDestroyEnvironment(Env);
        return;
    }
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][DecoderProbe] model loaded in %.1f ms"),
        (FPlatformTime::Seconds() - LoadStart) * 1000.0);

    // -----------------------------------------------------------------
    // 3. Enumerate signatures, log each, pick the smallest bucket.
    // -----------------------------------------------------------------
    LiteRtParamIndex NumSigs = 0;
    if (!CheckStatus(LiteRtGetNumModelSignatures(Model, &NumSigs),
                     TEXT("LiteRtGetNumModelSignatures")))
    {
        LiteRtDestroyModel(Model);
        LiteRtDestroyEnvironment(Env);
        return;
    }
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][DecoderProbe] %llu signatures advertised"),
        static_cast<uint64>(NumSigs));
    if (NumSigs == 0)
    {
        UE_LOG(LogInoAgents, Error,
            TEXT("[NeuTTS][DecoderProbe] model has no signatures — wrong file or broken export."));
        LiteRtDestroyModel(Model);
        LiteRtDestroyEnvironment(Env);
        return;
    }
    const LiteRtParamIndex SigIdx = EnumerateAndPickSmallestSignature(Model, NumSigs);

    // -----------------------------------------------------------------
    // 4. Compile (CPU accelerator).
    // -----------------------------------------------------------------
    LiteRtOptions Options = nullptr;
    if (!CheckStatus(LiteRtCreateOptions(&Options),
                     TEXT("LiteRtCreateOptions")))
    {
        LiteRtDestroyModel(Model);
        LiteRtDestroyEnvironment(Env);
        return;
    }
    CheckStatus(LiteRtSetOptionsHardwareAccelerators(Options, kLiteRtHwAcceleratorCpu),
                TEXT("LiteRtSetOptionsHardwareAccelerators"));

    LiteRtCompiledModel Compiled = nullptr;
    const double CompileStart = FPlatformTime::Seconds();
    if (!CheckStatus(LiteRtCreateCompiledModel(Env, Model, Options, &Compiled),
                     TEXT("LiteRtCreateCompiledModel")))
    {
        LiteRtDestroyOptions(Options);
        LiteRtDestroyModel(Model);
        LiteRtDestroyEnvironment(Env);
        return;
    }
    UE_LOG(LogInoAgents, Log,
        TEXT("[NeuTTS][DecoderProbe] compiled in %.1f ms"),
        (FPlatformTime::Seconds() - CompileStart) * 1000.0);

    // -----------------------------------------------------------------
    // 5. Get input + output tensor types and buffer requirements, then
    //    allocate managed buffers from those requirements.
    // -----------------------------------------------------------------
    LiteRtSignature Sig = nullptr;
    LiteRtGetModelSignature(Model, SigIdx, &Sig);

    LiteRtTensor InTensor = nullptr;
    LiteRtGetSignatureInputTensorByIndex(Sig, 0, &InTensor);
    LiteRtRankedTensorType InType = {};
    LiteRtGetRankedTensorType(InTensor, &InType);

    LiteRtTensor OutTensor = nullptr;
    LiteRtGetSignatureOutputTensorByIndex(Sig, 0, &OutTensor);
    LiteRtRankedTensorType OutType = {};
    LiteRtGetRankedTensorType(OutTensor, &OutType);

    LiteRtTensorBufferRequirements InReqs = nullptr;
    if (!CheckStatus(
            LiteRtGetCompiledModelInputBufferRequirements(Compiled, SigIdx, 0, &InReqs),
            TEXT("LiteRtGetCompiledModelInputBufferRequirements")))
    {
        LiteRtDestroyCompiledModel(Compiled);
        LiteRtDestroyOptions(Options);
        LiteRtDestroyModel(Model);
        LiteRtDestroyEnvironment(Env);
        return;
    }
    LiteRtTensorBufferRequirements OutReqs = nullptr;
    if (!CheckStatus(
            LiteRtGetCompiledModelOutputBufferRequirements(Compiled, SigIdx, 0, &OutReqs),
            TEXT("LiteRtGetCompiledModelOutputBufferRequirements")))
    {
        LiteRtDestroyCompiledModel(Compiled);
        LiteRtDestroyOptions(Options);
        LiteRtDestroyModel(Model);
        LiteRtDestroyEnvironment(Env);
        return;
    }

    LiteRtTensorBuffer InBuf = nullptr;
    if (!CheckStatus(
            LiteRtCreateManagedTensorBufferFromRequirements(Env, &InType, InReqs, &InBuf),
            TEXT("LiteRtCreateManagedTensorBufferFromRequirements (input)")))
    {
        LiteRtDestroyCompiledModel(Compiled);
        LiteRtDestroyOptions(Options);
        LiteRtDestroyModel(Model);
        LiteRtDestroyEnvironment(Env);
        return;
    }
    LiteRtTensorBuffer OutBuf = nullptr;
    if (!CheckStatus(
            LiteRtCreateManagedTensorBufferFromRequirements(Env, &OutType, OutReqs, &OutBuf),
            TEXT("LiteRtCreateManagedTensorBufferFromRequirements (output)")))
    {
        LiteRtDestroyTensorBuffer(InBuf);
        LiteRtDestroyCompiledModel(Compiled);
        LiteRtDestroyOptions(Options);
        LiteRtDestroyModel(Model);
        LiteRtDestroyEnvironment(Env);
        return;
    }

    // -----------------------------------------------------------------
    // 6. Fill the input tensor with random FSQ codes ∈ [0, 65535].
    //    The conversion script asserts the codes range; out-of-range
    //    values can produce NaN audio.
    // -----------------------------------------------------------------
    {
        size_t InPacked = 0;
        CheckStatus(LiteRtGetTensorBufferPackedSize(InBuf, &InPacked),
                    TEXT("LiteRtGetTensorBufferPackedSize (input)"));
        if (InType.element_type != kLiteRtElementTypeInt64)
        {
            UE_LOG(LogInoAgents, Warning,
                TEXT("[NeuTTS][DecoderProbe] input element_type=%s (expected int64). ")
                TEXT("This may indicate a stale .tflite export — continuing anyway."),
                ElementTypeName(InType.element_type));
        }

        void* InAddr = nullptr;
        if (!CheckStatus(LiteRtLockTensorBuffer(InBuf, &InAddr, kLiteRtTensorBufferLockModeWrite),
                         TEXT("LiteRtLockTensorBuffer (input)")))
        {
            LiteRtDestroyTensorBuffer(InBuf);
            LiteRtDestroyTensorBuffer(OutBuf);
            LiteRtDestroyCompiledModel(Compiled);
            LiteRtDestroyOptions(Options);
            LiteRtDestroyModel(Model);
            LiteRtDestroyEnvironment(Env);
            return;
        }
        const size_t NumElems = InPacked / sizeof(int64_t);
        int64_t* Data = static_cast<int64_t*>(InAddr);
        // FMath::Rand is 31-bit; mask to 16 bits for FSQ range.
        for (size_t i = 0; i < NumElems; ++i)
        {
            Data[i] = static_cast<int64_t>(FMath::Rand() & 0xFFFF);
        }
        CheckStatus(LiteRtUnlockTensorBuffer(InBuf),
                    TEXT("LiteRtUnlockTensorBuffer (input)"));

        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][DecoderProbe] input %s%s filled with %llu random codes in [0, 65535]"),
            ElementTypeName(InType.element_type), *FormatShape(InType),
            static_cast<uint64>(NumElems));
    }

    // -----------------------------------------------------------------
    // 7. Run one inference. THE crux of the spike.
    // -----------------------------------------------------------------
    LiteRtTensorBuffer Ins[1]  = { InBuf };
    LiteRtTensorBuffer Outs[1] = { OutBuf };
    const double RunStart = FPlatformTime::Seconds();
    if (!CheckStatus(LiteRtRunCompiledModel(Compiled, SigIdx, 1, Ins, 1, Outs),
                     TEXT("LiteRtRunCompiledModel")))
    {
        LiteRtDestroyTensorBuffer(InBuf);
        LiteRtDestroyTensorBuffer(OutBuf);
        LiteRtDestroyCompiledModel(Compiled);
        LiteRtDestroyOptions(Options);
        LiteRtDestroyModel(Model);
        LiteRtDestroyEnvironment(Env);
        return;
    }
    const double RunMs = (FPlatformTime::Seconds() - RunStart) * 1000.0;

    // -----------------------------------------------------------------
    // 8. Read the output: shape, total samples, min/max/mean, first 8 samples.
    // -----------------------------------------------------------------
    {
        size_t OutPacked = 0;
        CheckStatus(LiteRtGetTensorBufferPackedSize(OutBuf, &OutPacked),
                    TEXT("LiteRtGetTensorBufferPackedSize (output)"));
        if (OutType.element_type != kLiteRtElementTypeFloat32)
        {
            UE_LOG(LogInoAgents, Warning,
                TEXT("[NeuTTS][DecoderProbe] output element_type=%s (expected float32) — continuing anyway."),
                ElementTypeName(OutType.element_type));
        }

        void* OutAddr = nullptr;
        if (!CheckStatus(LiteRtLockTensorBuffer(OutBuf, &OutAddr, kLiteRtTensorBufferLockModeRead),
                         TEXT("LiteRtLockTensorBuffer (output)")))
        {
            LiteRtDestroyTensorBuffer(InBuf);
            LiteRtDestroyTensorBuffer(OutBuf);
            LiteRtDestroyCompiledModel(Compiled);
            LiteRtDestroyOptions(Options);
            LiteRtDestroyModel(Model);
            LiteRtDestroyEnvironment(Env);
            return;
        }

        const size_t NumSamples = OutPacked / sizeof(float);
        const float* Samples = static_cast<const float*>(OutAddr);

        float MinV = TNumericLimits<float>::Max();
        float MaxV = TNumericLimits<float>::Lowest();
        double Sum = 0.0;
        double AbsSum = 0.0;
        for (size_t i = 0; i < NumSamples; ++i)
        {
            const float v = Samples[i];
            MinV = FMath::Min(MinV, v);
            MaxV = FMath::Max(MaxV, v);
            Sum += v;
            AbsSum += FMath::Abs(v);
        }
        const double Mean = NumSamples > 0 ? Sum / static_cast<double>(NumSamples) : 0.0;
        const double AbsMean = NumSamples > 0 ? AbsSum / static_cast<double>(NumSamples) : 0.0;

        FString FirstSamples;
        for (size_t i = 0; i < FMath::Min<size_t>(NumSamples, 8); ++i)
        {
            if (i > 0) FirstSamples += TEXT(", ");
            FirstSamples += FString::Printf(TEXT("%.5f"), Samples[i]);
        }

        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][DecoderProbe] inference: %.1f ms  output %s%s  samples=%llu"),
            RunMs, ElementTypeName(OutType.element_type), *FormatShape(OutType),
            static_cast<uint64>(NumSamples));
        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][DecoderProbe] output stats: min=%.5f  max=%.5f  mean=%.5f  abs_mean=%.5f"),
            MinV, MaxV, static_cast<float>(Mean), static_cast<float>(AbsMean));
        UE_LOG(LogInoAgents, Log,
            TEXT("[NeuTTS][DecoderProbe] first samples: [%s]"), *FirstSamples);

        // A trivial sanity check: random codes through a real decoder
        // should produce non-trivial output. AbsMean ~ 0 strongly
        // suggests a zero buffer (wrong API call, dead delegate, etc).
        if (AbsMean < 1e-7)
        {
            UE_LOG(LogInoAgents, Error,
                TEXT("[NeuTTS][DecoderProbe] output abs-mean is ~0. Either the model didn't run, ")
                TEXT("the output tensor wasn't populated, or there's a delegate / accelerator wiring issue."));
        }

        CheckStatus(LiteRtUnlockTensorBuffer(OutBuf),
                    TEXT("LiteRtUnlockTensorBuffer (output)"));
    }

    // -----------------------------------------------------------------
    // 9. Cleanup.
    // -----------------------------------------------------------------
    LiteRtDestroyTensorBuffer(InBuf);
    LiteRtDestroyTensorBuffer(OutBuf);
    LiteRtDestroyCompiledModel(Compiled);
    LiteRtDestroyOptions(Options);
    LiteRtDestroyModel(Model);
    LiteRtDestroyEnvironment(Env);

    UE_LOG(LogInoAgents, Log, TEXT("[NeuTTS][DecoderProbe] === END ==="));
}

static FAutoConsoleCommand GDecoderProbeCmd(
    TEXT("Ino.NeuTTS.DecoderProbeTest"),
    TEXT("Phase 0a spike — load a NeuCodec .tflite via the bare LiteRT C API, run one ")
    TEXT("inference with random FSQ codes, log output shape + stats. ")
    TEXT("Args: <abs path to .tflite>"),
    FConsoleCommandWithArgsDelegate::CreateStatic(&RunDecoderProbeTest));

} // namespace
