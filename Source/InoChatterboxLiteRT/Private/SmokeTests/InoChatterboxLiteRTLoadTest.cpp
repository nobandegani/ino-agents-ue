// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "ChatterboxLiteRT/InoChatterboxLiteRTEnv.h"
#include "ChatterboxLiteRT/InoChatterboxLiteRTModel.h"
#include "ChatterboxLiteRT/InoChatterboxLiteRTTensor.h"
#include "InoChatterboxLiteRT.h"

#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
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
#if !(PLATFORM_WINDOWS || PLATFORM_ANDROID)
        UE_LOG(LogInoChatterboxLiteRT, Warning,
            TEXT("Ino.ChatterboxLiteRT.LoadTest: not supported on this platform "
                 "— InoLiteRT ships only Win64 + Android."));
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

        // Allocate input: int32[1, 3] (matches the dtype text_emb expects;
        // verified by the metadata dump above — text_emb's input is int32).
        FInoChatterboxLiteRTTensor InputBuf;
        const int32 InputDims[] = { 1, 3 };
        if (!InputBuf.CreateManagedHost(Env, kLiteRtElementTypeInt32,
                TArrayView<const int32>(InputDims, 2)))
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LoadTest: failed to allocate text_emb input buffer."));
            return;
        }
        if (int32* Ptr = static_cast<int32*>(InputBuf.LockForWrite()))
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

        // Allocate output: f32[1, 3, 1024] (text embedding hidden_size=1024
        // for chatterbox-turbo per the architecture constants).
        FInoChatterboxLiteRTTensor OutputBuf;
        const int32 OutputDims[] = { 1, 3, 1024 };
        if (!OutputBuf.CreateManagedHost(Env, kLiteRtElementTypeFloat32,
                TArrayView<const int32>(OutputDims, 3)))
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
        float MinV = OutPtr[0];
        float MaxV = OutPtr[0];
        for (int32 i = 0; i < NumElems; ++i)
        {
            const float V = OutPtr[i];
            if (V != 0.0f) { ++NonZeroCount; }
            if (V < MinV) { MinV = V; }
            if (V > MaxV) { MaxV = V; }
        }
        OutputBuf.Unlock();

        UE_LOG(LogInoChatterboxLiteRT, Log,
            TEXT("LoadTest: text_emb forward done in %.4fs — output[1,3,1024] fp32, "
                 "non_zero=%d/%d, range=[%.4f, %.4f]"),
            Elapsed, NonZeroCount, NumElems, MinV, MaxV);

        if (NonZeroCount == 0)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LoadTest: FAIL — text_emb output is all zeros (model not actually running?)."));
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
