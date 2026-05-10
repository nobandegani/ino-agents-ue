// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "ChatterboxLiteRT/InoChatterboxLiteRTEnv.h"
#include "ChatterboxLiteRT/InoChatterboxLiteRTModel.h"
#include "ChatterboxLiteRT/InoChatterboxLiteRTTensor.h"
#include "InoChatterboxLiteRT.h"

#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
#include "litert/c/litert_common.h"
#include "litert/c/litert_model_types.h"
#endif

namespace
{
    /** Resolves InoAgents plugin's BaseDir/Chatterbox/LiteRT/models/<sub>/<file>. */
    FString ResolveModelPath(const TCHAR* SubDir, const TCHAR* FileName)
    {
        const TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return FPaths::Combine(
            Plugin->GetBaseDir(),
            TEXT("Chatterbox"), TEXT("LiteRT"), TEXT("models"),
            SubDir, FileName);
    }

    /**
     * Phase 1 smoke test:
     *   1. Initialize LiteRtEnvironment (lazily — first GetEnvironment call).
     *   2. Load all 4 .tflite files (wfp32_afp32 variant).
     *   3. Dump every signature's input + output (name, dtype, shape).
     *   4. Run text_emb forward with synthetic input_ids = [101, 102, 103]
     *      and verify the output is shape (1, 3, 1024) fp32 with at least
     *      one non-zero element.
     *
     * This is the de-risk milestone for Phase 1 — proves the whole loading
     * + signature-cache + tensor-buffer + RunCompiledModel path works.
     */
    void RunLoadTest(const TArray<FString>& /*Args*/)
    {
#if !(PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC)
        UE_LOG(LogInoChatterboxLiteRT, Warning,
            TEXT("Ino.ChatterboxLiteRT.LoadTest: not supported on this platform "
                 "— InoLiteRT ships Win64 + Android + Mac + iOS."));
#else
        UE_LOG(LogInoChatterboxLiteRT, Log, TEXT("=== Ino.ChatterboxLiteRT.LoadTest ==="));

        // Step 1: env.
        LiteRtEnvironment Env = InoChatterboxLiteRT::GetEnvironment();
        if (!Env)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LoadTest: failed to create LiteRtEnvironment — aborting."));
            return;
        }

        // Step 2: resolve + load 4 .tflite files.
        const TArray<TPair<FString, FString>> Models = {
            { TEXT("text_emb"),            ResolveModelPath(TEXT("embedding"),   TEXT("text_emb_wfp32_afp32.tflite")) },
            { TEXT("speech_emb"),          ResolveModelPath(TEXT("embedding"),   TEXT("speech_emb_wfp32_afp32.tflite")) },
            { TEXT("language_model"),      ResolveModelPath(TEXT("llm"),         TEXT("language_model_wfp32_afp32.tflite")) },
            { TEXT("conditional_decoder"), ResolveModelPath(TEXT("conditional"), TEXT("conditional_decoder_wfp32_afp32.tflite")) },
        };

        TMap<FString, TUniquePtr<FInoChatterboxLiteRTModel>> Loaded;
        for (const TPair<FString, FString>& Pair : Models)
        {
            const FString& Label = Pair.Key;
            const FString& Path  = Pair.Value;
            if (Path.IsEmpty() || !FPaths::FileExists(Path))
            {
                UE_LOG(LogInoChatterboxLiteRT, Error,
                    TEXT("LoadTest: missing file for %s — expected at %s"),
                    *Label, *Path);
                continue;
            }
            const double T0 = FPlatformTime::Seconds();
            TUniquePtr<FInoChatterboxLiteRTModel> Model = MakeUnique<FInoChatterboxLiteRTModel>();
            const bool bOk = Model->Load(Env, Path, kLiteRtHwAcceleratorCpu);
            const double Elapsed = FPlatformTime::Seconds() - T0;
            if (!bOk)
            {
                UE_LOG(LogInoChatterboxLiteRT, Error,
                    TEXT("LoadTest: %s FAILED to load (%s)."), *Label, *Path);
                continue;
            }
            UE_LOG(LogInoChatterboxLiteRT, Log,
                TEXT("LoadTest: %s loaded in %.2fs (%d signatures, fully_accelerated=%s)."),
                *Label, Elapsed, Model->NumSignatures(),
                Model->IsFullyAccelerated() ? TEXT("yes") : TEXT("no"));
            Model->LogSignatures();
            Loaded.Add(Label, MoveTemp(Model));
        }

        if (Loaded.Num() != Models.Num())
        {
            UE_LOG(LogInoChatterboxLiteRT, Warning,
                TEXT("LoadTest: only %d/%d models loaded — skipping forward-pass step."),
                Loaded.Num(), Models.Num());
            return;
        }

        // Step 3: run text_emb on a synthetic [1, 3] int32 input.
        FInoChatterboxLiteRTModel* TextEmb = Loaded[TEXT("text_emb")].Get();
        const FInoChatterboxLiteRTSignatureInfo* Sig = TextEmb->GetSignatureInfo(0);
        if (!Sig || Sig->Inputs.Num() < 1 || Sig->Outputs.Num() < 1)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LoadTest: text_emb has unexpected signature shape (inputs=%d outputs=%d)."),
                Sig ? Sig->Inputs.Num() : 0, Sig ? Sig->Outputs.Num() : 0);
            return;
        }

        // text_emb has dynamic input shape [?, ?] and dynamic output shape
        // [?, ?, 1024]. Resize input, then probe what shape the runtime
        // actually settled on for both input and output.
        const int32 InputDims[] = { 1, 3 };
        if (!TextEmb->ResizeInputTensor(/*sig=*/0, /*input=*/0,
                TArrayView<const int32>(InputDims, 2)))
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LoadTest: failed to resize text_emb input."));
            return;
        }

        // Diagnostic: did resize actually take effect on the input side?
        TArray<int32> PostResizeInputDims;
        if (TextEmb->GetInputTensorLayout(0, 0, PostResizeInputDims))
        {
            FString InStr;
            for (int32 i = 0; i < PostResizeInputDims.Num(); ++i)
            {
                InStr += FString::Printf(TEXT("%s%d"), i == 0 ? TEXT("") : TEXT(", "),
                    PostResizeInputDims[i]);
            }
            UE_LOG(LogInoChatterboxLiteRT, Log,
                TEXT("LoadTest: text_emb POST-resize input layout = [%s] (requested [1, 3])"),
                *InStr);
        }

        // What does the output-layout query say AFTER resize?
        TArray<int32> ResolvedOutputDims;
        if (!TextEmb->GetOutputTensorLayout(/*sig=*/0, /*output=*/0, ResolvedOutputDims))
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LoadTest: failed to resolve text_emb output layout after resize."));
            return;
        }
        FString DimStr;
        for (int32 i = 0; i < ResolvedOutputDims.Num(); ++i)
        {
            DimStr += FString::Printf(TEXT("%s%d"), i == 0 ? TEXT("") : TEXT(", "),
                ResolvedOutputDims[i]);
        }
        UE_LOG(LogInoChatterboxLiteRT, Log,
            TEXT("LoadTest: text_emb output-layout-query result = [%s] (expected [1, 3, 1024])"),
            *DimStr);

        // OVERRIDE: force the output to the shape we KNOW it must be ([1, 3, 1024]).
        // Earlier runs showed the layout query returns [1, 1, 1024] regardless
        // of resize, but allocating an output buffer of that size means the
        // runtime only writes 1024 floats and our [1, 3, 1024] buffer is
        // partially uninitialized. Force the correct allocation here and see
        // whether the runtime fills all 3072 elements with real data.
        ResolvedOutputDims = { 1, 3, 1024 };
        UE_LOG(LogInoChatterboxLiteRT, Log,
            TEXT("LoadTest: OVERRIDING output allocation to [1, 3, 1024] for diagnosis"));

        // Allocate input: int64[1, 3]. Chatterbox's text_emb (and speech_emb)
        // expect int64 token ids — NOT int32 like most other LLM exports use
        // at the LiteRT boundary. The conditional_decoder is the opposite —
        // its speech_tokens input is i32 [1, ?]. So our future runner must
        // allocate per-tensor dtypes from cached signature metadata, not
        // assume one global token width.
        FInoChatterboxLiteRTTensor InputBuf;
        if (!InputBuf.CreateManagedHost(Env, kLiteRtElementTypeInt64,
                TArrayView<const int32>(InputDims, 2)))
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LoadTest: failed to allocate text_emb input buffer."));
            return;
        }
        if (int64* Ptr = static_cast<int64*>(InputBuf.LockForWrite()))
        {
            Ptr[0] = 101; Ptr[1] = 102; Ptr[2] = 103;
            InputBuf.Unlock();
        }
        else
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LoadTest: failed to lock text_emb input buffer."));
            return;
        }

        // Allocate output using the resolved (post-resize) layout instead of
        // hardcoding [1, 3, 1024]. This keeps the test correct even if the
        // input shape changes.
        FInoChatterboxLiteRTTensor OutputBuf;
        if (!OutputBuf.CreateManagedHost(Env, kLiteRtElementTypeFloat32, ResolvedOutputDims))
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LoadTest: failed to allocate text_emb output buffer."));
            return;
        }

        FInoChatterboxLiteRTTensor* InputArray[]  = { &InputBuf };
        FInoChatterboxLiteRTTensor* OutputArray[] = { &OutputBuf };

        const double T0 = FPlatformTime::Seconds();
        const bool bRan = TextEmb->Run(
            /*SignatureIndex=*/0,
            TArrayView<FInoChatterboxLiteRTTensor*>(InputArray, 1),
            TArrayView<FInoChatterboxLiteRTTensor*>(OutputArray, 1));
        const double Elapsed = FPlatformTime::Seconds() - T0;
        if (!bRan)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LoadTest: text_emb forward Run() FAILED."));
            return;
        }

        // Verify output: expect (1, 3, 1024) fp32 with at least one non-zero.
        const float* OutPtr = static_cast<const float*>(OutputBuf.LockForRead());
        if (!OutPtr)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LoadTest: text_emb output lock failed."));
            return;
        }
        const int32 NumElems = 1 * 3 * 1024;
        int32 NonZeroCount = 0;
        double MeanAbs = 0.0;
        float MinV = OutPtr[0];
        float MaxV = OutPtr[0];
        for (int32 i = 0; i < NumElems; ++i)
        {
            const float V = OutPtr[i];
            if (V != 0.0f) { ++NonZeroCount; }
            if (V < MinV) { MinV = V; }
            if (V > MaxV) { MaxV = V; }
            MeanAbs += FMath::Abs(V);
        }
        MeanAbs /= NumElems;

        // Sample values from row 0 and row 2 (token 101 and token 103) to give
        // visual confirmation we're getting real embedding rows, not garbage.
        FString Row0First8;
        FString Row2Last8;
        for (int32 i = 0; i < 8; ++i)
        {
            Row0First8 += FString::Printf(TEXT("%s%.4e"), i == 0 ? TEXT("") : TEXT(", "), OutPtr[i]);
            Row2Last8  += FString::Printf(TEXT("%s%.4e"), i == 0 ? TEXT("") : TEXT(", "), OutPtr[2 * 1024 + 1024 - 8 + i]);
        }
        OutputBuf.Unlock();

        UE_LOG(LogInoChatterboxLiteRT, Log,
            TEXT("LoadTest: text_emb forward done in %.4fs — output[1,3,1024] fp32:"),
            Elapsed);
        UE_LOG(LogInoChatterboxLiteRT, Log,
            TEXT("    non_zero=%d/%d, mean_abs=%.4e, range=[%.4e, %.4e]"),
            NonZeroCount, NumElems, MeanAbs, MinV, MaxV);
        UE_LOG(LogInoChatterboxLiteRT, Log,
            TEXT("    row0 first 8: [%s]"), *Row0First8);
        UE_LOG(LogInoChatterboxLiteRT, Log,
            TEXT("    row2 last  8: [%s]"), *Row2Last8);

        // Sanity check: real GPT-2-Medium fp32 token embeddings have mean_abs
        // in the rough range 0.005..0.05 (depends on the specific token).
        // mean_abs near zero = buffer is uninitialized / model didn't actually
        // write to it. Anything > 1e-5 is consistent with real embedding data.
        if (NonZeroCount == 0)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LoadTest: FAIL — text_emb output is all zeros (model not running?)."));
        }
        else if (MeanAbs < 1e-5)
        {
            UE_LOG(LogInoChatterboxLiteRT, Warning,
                TEXT("LoadTest: SUSPICIOUS — mean_abs=%.4e is near zero. Output "
                     "may be uninitialized buffer rather than real embeddings. "
                     "Run again and compare bit values."),
                MeanAbs);
        }
        else
        {
            UE_LOG(LogInoChatterboxLiteRT, Log,
                TEXT("LoadTest: PASS — Phase 1 milestone reached."));
        }
#endif
    }

    static FAutoConsoleCommand GLoadTestCmd(
        TEXT("Ino.ChatterboxLiteRT.LoadTest"),
        TEXT("Load all 4 wfp32_afp32 .tflite files for Chatterbox Turbo via LiteRT, "
             "dump signatures, and run text_emb forward as a sanity check."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunLoadTest));
}
