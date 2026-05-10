// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxLiteRTModel.h"

#include "InoChatterboxLiteRT.h"
#include "InoChatterboxLiteRTTensor.h"

#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
#include "litert/c/litert_compiled_model.h"
#include "litert/c/litert_environment.h"
#include "litert/c/litert_model.h"
#include "litert/c/litert_options.h"
#endif

FInoChatterboxLiteRTModel::FInoChatterboxLiteRTModel(FInoChatterboxLiteRTModel&& Other) noexcept
{
#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
    Model = Other.Model;
    CompiledModel = Other.CompiledModel;
    Other.Model = nullptr;
    Other.CompiledModel = nullptr;
#endif
    ModelPath = MoveTemp(Other.ModelPath);
    Signatures = MoveTemp(Other.Signatures);
    SignatureIndexByName = MoveTemp(Other.SignatureIndexByName);
}

FInoChatterboxLiteRTModel& FInoChatterboxLiteRTModel::operator=(FInoChatterboxLiteRTModel&& Other) noexcept
{
    if (this == &Other)
    {
        return *this;
    }
#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
    Reset();
    Model = Other.Model;
    CompiledModel = Other.CompiledModel;
    Other.Model = nullptr;
    Other.CompiledModel = nullptr;
#endif
    ModelPath = MoveTemp(Other.ModelPath);
    Signatures = MoveTemp(Other.Signatures);
    SignatureIndexByName = MoveTemp(Other.SignatureIndexByName);
    return *this;
}

FInoChatterboxLiteRTModel::~FInoChatterboxLiteRTModel()
{
#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
    Reset();
#endif
}

#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC

void FInoChatterboxLiteRTModel::Reset()
{
    if (CompiledModel)
    {
        LiteRtDestroyCompiledModel(CompiledModel);
        CompiledModel = nullptr;
    }
    if (Model)
    {
        LiteRtDestroyModel(Model);
        Model = nullptr;
    }
    Signatures.Reset();
    SignatureIndexByName.Reset();
    ModelPath.Reset();
}

bool FInoChatterboxLiteRTModel::Load(
    LiteRtEnvironment Env,
    const FString& AbsolutePath,
    int32 HardwareAcceleratorMask)
{
    Reset();

    if (!Env)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("Load(%s): null environment."), *AbsolutePath);
        return false;
    }

    // Step 1: read the .tflite file off disk into a LiteRtModel.
    {
        FTCHARToUTF8 PathUtf8(*AbsolutePath);
        const LiteRtStatus Status = LiteRtCreateModelFromFile(PathUtf8.Get(), &Model);
        if (Status != kLiteRtStatusOk || !Model)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LiteRtCreateModelFromFile failed for '%s': status=%d (%s)."),
                *AbsolutePath,
                static_cast<int32>(Status),
                *InoChatterboxLiteRT::StatusToString(Status));
            Model = nullptr;
            return false;
        }
    }

    // Step 2: build LiteRtOptions with the requested HW accelerator mask.
    LiteRtOptions Options = nullptr;
    {
        const LiteRtStatus Status = LiteRtCreateOptions(&Options);
        if (Status != kLiteRtStatusOk || !Options)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LiteRtCreateOptions failed for '%s': status=%d (%s)."),
                *AbsolutePath,
                static_cast<int32>(Status),
                *InoChatterboxLiteRT::StatusToString(Status));
            Reset();
            return false;
        }
    }
    {
        const LiteRtStatus Status = LiteRtSetOptionsHardwareAccelerators(
            Options, static_cast<LiteRtHwAcceleratorSet>(HardwareAcceleratorMask));
        if (Status != kLiteRtStatusOk)
        {
            UE_LOG(LogInoChatterboxLiteRT, Warning,
                TEXT("LiteRtSetOptionsHardwareAccelerators(0x%x) failed for '%s': status=%d (%s) — proceeding anyway."),
                HardwareAcceleratorMask,
                *AbsolutePath,
                static_cast<int32>(Status),
                *InoChatterboxLiteRT::StatusToString(Status));
        }
    }

    // Step 3: compile.
    {
        const LiteRtStatus Status = LiteRtCreateCompiledModel(
            Env, Model, Options, &CompiledModel);
        // Options are owned by the caller per the API; release them either way.
        LiteRtDestroyOptions(Options);
        Options = nullptr;
        if (Status != kLiteRtStatusOk || !CompiledModel)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LiteRtCreateCompiledModel failed for '%s': status=%d (%s)."),
                *AbsolutePath,
                static_cast<int32>(Status),
                *InoChatterboxLiteRT::StatusToString(Status));
            Reset();
            return false;
        }
    }

    ModelPath = AbsolutePath;

    // Step 4: cache signature + I/O metadata for O(1) per-call lookups.
    if (!BuildSignatureCache())
    {
        Reset();
        return false;
    }

    return true;
}

bool FInoChatterboxLiteRTModel::BuildSignatureCache()
{
    LiteRtParamIndex NumSigs = 0;
    {
        const LiteRtStatus Status = LiteRtGetNumModelSignatures(Model, &NumSigs);
        if (Status != kLiteRtStatusOk)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LiteRtGetNumModelSignatures failed: status=%d (%s)."),
                static_cast<int32>(Status),
                *InoChatterboxLiteRT::StatusToString(Status));
            return false;
        }
    }
    if (NumSigs == 0)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("Model '%s' has 0 signatures — nothing callable."),
            *ModelPath);
        return false;
    }

    Signatures.Reserve(static_cast<int32>(NumSigs));
    SignatureIndexByName.Reserve(static_cast<int32>(NumSigs));

    for (LiteRtParamIndex SigIdx = 0; SigIdx < NumSigs; ++SigIdx)
    {
        LiteRtSignature Sig = nullptr;
        if (LiteRtGetModelSignature(Model, SigIdx, &Sig) != kLiteRtStatusOk || !Sig)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LiteRtGetModelSignature(%llu) failed."),
                static_cast<uint64>(SigIdx));
            return false;
        }

        const char* KeyAnsi = nullptr;
        if (LiteRtGetSignatureKey(Sig, &KeyAnsi) != kLiteRtStatusOk || !KeyAnsi)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LiteRtGetSignatureKey(%llu) failed."),
                static_cast<uint64>(SigIdx));
            return false;
        }

        FInoChatterboxLiteRTSignatureInfo Info;
        Info.Index = static_cast<int32>(SigIdx);
        Info.Key = FName(ANSI_TO_TCHAR(KeyAnsi));

        // Inputs.
        LiteRtParamIndex NumInputs = 0;
        if (LiteRtGetNumSignatureInputs(Sig, &NumInputs) != kLiteRtStatusOk)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LiteRtGetNumSignatureInputs(%s) failed."), *Info.Key.ToString());
            return false;
        }
        Info.Inputs.Reserve(static_cast<int32>(NumInputs));
        Info.InputIndexByName.Reserve(static_cast<int32>(NumInputs));
        for (LiteRtParamIndex InIdx = 0; InIdx < NumInputs; ++InIdx)
        {
            const char* NameAnsi = nullptr;
            if (LiteRtGetSignatureInputName(Sig, InIdx, &NameAnsi) != kLiteRtStatusOk || !NameAnsi)
            {
                UE_LOG(LogInoChatterboxLiteRT, Error,
                    TEXT("LiteRtGetSignatureInputName(%s, %llu) failed."),
                    *Info.Key.ToString(), static_cast<uint64>(InIdx));
                return false;
            }
            FInoChatterboxLiteRTTensorMeta Meta;
            Meta.Name = FName(ANSI_TO_TCHAR(NameAnsi));

            LiteRtTensor T = nullptr;
            if (LiteRtGetSignatureInputTensorByIndex(Sig, InIdx, &T) == kLiteRtStatusOk && T)
            {
                LiteRtRankedTensorType Ranked;
                FMemory::Memzero(Ranked);
                if (LiteRtGetRankedTensorType(T, &Ranked) == kLiteRtStatusOk)
                {
                    Meta.ElementType = Ranked.element_type;
                    for (unsigned int d = 0; d < Ranked.layout.rank; ++d)
                    {
                        Meta.Dims.Add(Ranked.layout.dimensions[d]);
                    }
                }
            }
            const int32 PositionalIdx = static_cast<int32>(InIdx);
            Info.InputIndexByName.Add(Meta.Name, PositionalIdx);
            Info.Inputs.Add(MoveTemp(Meta));
        }

        // Outputs.
        LiteRtParamIndex NumOutputs = 0;
        if (LiteRtGetNumSignatureOutputs(Sig, &NumOutputs) != kLiteRtStatusOk)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LiteRtGetNumSignatureOutputs(%s) failed."), *Info.Key.ToString());
            return false;
        }
        Info.Outputs.Reserve(static_cast<int32>(NumOutputs));
        Info.OutputIndexByName.Reserve(static_cast<int32>(NumOutputs));
        for (LiteRtParamIndex OutIdx = 0; OutIdx < NumOutputs; ++OutIdx)
        {
            const char* NameAnsi = nullptr;
            if (LiteRtGetSignatureOutputName(Sig, OutIdx, &NameAnsi) != kLiteRtStatusOk || !NameAnsi)
            {
                UE_LOG(LogInoChatterboxLiteRT, Error,
                    TEXT("LiteRtGetSignatureOutputName(%s, %llu) failed."),
                    *Info.Key.ToString(), static_cast<uint64>(OutIdx));
                return false;
            }
            FInoChatterboxLiteRTTensorMeta Meta;
            Meta.Name = FName(ANSI_TO_TCHAR(NameAnsi));

            LiteRtTensor T = nullptr;
            if (LiteRtGetSignatureOutputTensorByIndex(Sig, OutIdx, &T) == kLiteRtStatusOk && T)
            {
                LiteRtRankedTensorType Ranked;
                FMemory::Memzero(Ranked);
                if (LiteRtGetRankedTensorType(T, &Ranked) == kLiteRtStatusOk)
                {
                    Meta.ElementType = Ranked.element_type;
                    for (unsigned int d = 0; d < Ranked.layout.rank; ++d)
                    {
                        Meta.Dims.Add(Ranked.layout.dimensions[d]);
                    }
                }
            }
            const int32 PositionalIdx = static_cast<int32>(OutIdx);
            Info.OutputIndexByName.Add(Meta.Name, PositionalIdx);
            Info.Outputs.Add(MoveTemp(Meta));
        }

        SignatureIndexByName.Add(Info.Key, Info.Index);
        Signatures.Add(MoveTemp(Info));
    }

    return true;
}

int32 FInoChatterboxLiteRTModel::GetSignatureIndex(FName SignatureKey) const
{
    if (const int32* Found = SignatureIndexByName.Find(SignatureKey))
    {
        return *Found;
    }
    return INDEX_NONE;
}

const FInoChatterboxLiteRTSignatureInfo* FInoChatterboxLiteRTModel::GetSignatureInfo(int32 SignatureIndex) const
{
    if (Signatures.IsValidIndex(SignatureIndex))
    {
        return &Signatures[SignatureIndex];
    }
    return nullptr;
}

const FInoChatterboxLiteRTSignatureInfo* FInoChatterboxLiteRTModel::GetSignatureInfo(FName SignatureKey) const
{
    return GetSignatureInfo(GetSignatureIndex(SignatureKey));
}

int32 FInoChatterboxLiteRTModel::GetInputIndex(int32 SignatureIndex, FName InputName) const
{
    if (const FInoChatterboxLiteRTSignatureInfo* Info = GetSignatureInfo(SignatureIndex))
    {
        if (const int32* Found = Info->InputIndexByName.Find(InputName))
        {
            return *Found;
        }
    }
    return INDEX_NONE;
}

int32 FInoChatterboxLiteRTModel::GetOutputIndex(int32 SignatureIndex, FName OutputName) const
{
    if (const FInoChatterboxLiteRTSignatureInfo* Info = GetSignatureInfo(SignatureIndex))
    {
        if (const int32* Found = Info->OutputIndexByName.Find(OutputName))
        {
            return *Found;
        }
    }
    return INDEX_NONE;
}

bool FInoChatterboxLiteRTModel::Run(
    int32 SignatureIndex,
    TArrayView<FInoChatterboxLiteRTTensor*> Inputs,
    TArrayView<FInoChatterboxLiteRTTensor*> Outputs)
{
    if (!CompiledModel)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("Run: model not loaded."));
        return false;
    }
    const FInoChatterboxLiteRTSignatureInfo* Info = GetSignatureInfo(SignatureIndex);
    if (!Info)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("Run: invalid signature index %d."), SignatureIndex);
        return false;
    }
    if (Inputs.Num() != Info->Inputs.Num())
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("Run(%s): input count %d != expected %d."),
            *Info->Key.ToString(), Inputs.Num(), Info->Inputs.Num());
        return false;
    }
    if (Outputs.Num() != Info->Outputs.Num())
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("Run(%s): output count %d != expected %d."),
            *Info->Key.ToString(), Outputs.Num(), Info->Outputs.Num());
        return false;
    }

    TArray<LiteRtTensorBuffer, TInlineAllocator<8>> InHandles;
    InHandles.Reserve(Inputs.Num());
    for (int32 i = 0; i < Inputs.Num(); ++i)
    {
        if (!Inputs[i] || !Inputs[i]->Get())
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("Run(%s): input %d (%s) is null/unallocated."),
                *Info->Key.ToString(), i, *Info->Inputs[i].Name.ToString());
            return false;
        }
        InHandles.Add(Inputs[i]->Get());
    }
    TArray<LiteRtTensorBuffer, TInlineAllocator<8>> OutHandles;
    OutHandles.Reserve(Outputs.Num());
    for (int32 i = 0; i < Outputs.Num(); ++i)
    {
        if (!Outputs[i] || !Outputs[i]->Get())
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("Run(%s): output %d (%s) is null/unallocated."),
                *Info->Key.ToString(), i, *Info->Outputs[i].Name.ToString());
            return false;
        }
        OutHandles.Add(Outputs[i]->Get());
    }

    const LiteRtStatus Status = LiteRtRunCompiledModel(
        CompiledModel,
        static_cast<LiteRtParamIndex>(SignatureIndex),
        InHandles.Num(), InHandles.GetData(),
        OutHandles.Num(), OutHandles.GetData());
    if (Status != kLiteRtStatusOk)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("LiteRtRunCompiledModel(%s) failed: status=%d (%s)."),
            *Info->Key.ToString(),
            static_cast<int32>(Status),
            *InoChatterboxLiteRT::StatusToString(Status));
        return false;
    }
    return true;
}

bool FInoChatterboxLiteRTModel::IsFullyAccelerated() const
{
    if (!CompiledModel)
    {
        return false;
    }
    bool bAccel = false;
    if (LiteRtCompiledModelIsFullyAccelerated(CompiledModel, &bAccel) != kLiteRtStatusOk)
    {
        return false;
    }
    return bAccel;
}

bool FInoChatterboxLiteRTModel::ResizeInputTensor(
    int32 SignatureIndex,
    int32 InputIndex,
    TArrayView<const int32> NewDims)
{
    if (!CompiledModel)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("ResizeInputTensor: model not loaded."));
        return false;
    }
    if (NewDims.Num() == 0)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("ResizeInputTensor: empty dims array."));
        return false;
    }
    // Use the NonStrict variant — its docs explicitly state it "should be
    // paired with LiteRtGetCompiledModelOutputTensorLayouts(...,
    // update_allocation=true) to propagate shape changes to outputs", which
    // is exactly the pattern our runner needs. The strict variant does not
    // reliably propagate shape changes through the compiled model's output
    // layout query for the chatterbox-turbo text_emb / speech_emb / decoder
    // graphs (verified empirically: strict resize to [1, 3] still resolved
    // output to [1, 1, 1024] instead of the expected [1, 3, 1024], causing
    // RunCompiledModel to fail with kLiteRtStatusErrorRuntimeFailure).
    const LiteRtStatus Status = LiteRtCompiledModelResizeInputTensorNonStrict(
        CompiledModel,
        static_cast<LiteRtParamIndex>(SignatureIndex),
        static_cast<LiteRtParamIndex>(InputIndex),
        NewDims.GetData(),
        static_cast<size_t>(NewDims.Num()));
    if (Status != kLiteRtStatusOk)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("LiteRtCompiledModelResizeInputTensorNonStrict(sig=%d, in=%d) failed: %s."),
            SignatureIndex, InputIndex,
            *InoChatterboxLiteRT::StatusToString(Status));
        return false;
    }
    return true;
}

bool FInoChatterboxLiteRTModel::GetOutputTensorLayout(
    int32 SignatureIndex,
    int32 OutputIndex,
    TArray<int32>& OutDims,
    bool bUpdateAllocation)
{
    OutDims.Reset();
    if (!CompiledModel)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("GetOutputTensorLayout: model not loaded."));
        return false;
    }
    const FInoChatterboxLiteRTSignatureInfo* Info = GetSignatureInfo(SignatureIndex);
    if (!Info)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("GetOutputTensorLayout: invalid signature index %d."), SignatureIndex);
        return false;
    }
    if (!Info->Outputs.IsValidIndex(OutputIndex))
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("GetOutputTensorLayout: invalid output index %d (signature has %d)."),
            OutputIndex, Info->Outputs.Num());
        return false;
    }

    // The All-Outputs form requires us to allocate enough Layout slots for
    // every output; it then writes them all in one call. We only care about
    // OutputIndex but pulling all outputs at once is the only API surface.
    TArray<LiteRtLayout, TInlineAllocator<8>> Layouts;
    Layouts.SetNumZeroed(Info->Outputs.Num());
    const LiteRtStatus Status = LiteRtGetCompiledModelOutputTensorLayouts(
        CompiledModel,
        static_cast<LiteRtParamIndex>(SignatureIndex),
        static_cast<size_t>(Layouts.Num()),
        Layouts.GetData(),
        bUpdateAllocation);
    if (Status != kLiteRtStatusOk)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("LiteRtGetCompiledModelOutputTensorLayouts(sig=%d) failed: %s."),
            SignatureIndex,
            *InoChatterboxLiteRT::StatusToString(Status));
        return false;
    }

    const LiteRtLayout& L = Layouts[OutputIndex];
    OutDims.Reserve(L.rank);
    for (unsigned int i = 0; i < L.rank; ++i)
    {
        OutDims.Add(L.dimensions[i]);
    }
    return true;
}

bool FInoChatterboxLiteRTModel::GetInputTensorLayout(
    int32 SignatureIndex,
    int32 InputIndex,
    TArray<int32>& OutDims)
{
    OutDims.Reset();
    if (!CompiledModel)
    {
        return false;
    }
    LiteRtLayout L;
    FMemory::Memzero(L);
    const LiteRtStatus Status = LiteRtGetCompiledModelInputTensorLayout(
        CompiledModel,
        static_cast<LiteRtParamIndex>(SignatureIndex),
        static_cast<LiteRtParamIndex>(InputIndex),
        &L);
    if (Status != kLiteRtStatusOk)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("LiteRtGetCompiledModelInputTensorLayout(sig=%d, in=%d) failed: %s."),
            SignatureIndex, InputIndex,
            *InoChatterboxLiteRT::StatusToString(Status));
        return false;
    }
    OutDims.Reserve(L.rank);
    for (unsigned int i = 0; i < L.rank; ++i)
    {
        OutDims.Add(L.dimensions[i]);
    }
    return true;
}

#endif  // PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC

void FInoChatterboxLiteRTModel::LogSignatures() const
{
    UE_LOG(LogInoChatterboxLiteRT, Log,
        TEXT("--- Model: %s (signatures=%d) ---"),
        *ModelPath, Signatures.Num());
    for (const FInoChatterboxLiteRTSignatureInfo& Info : Signatures)
    {
        UE_LOG(LogInoChatterboxLiteRT, Log,
            TEXT("  signature[%d] '%s'  inputs=%d outputs=%d"),
            Info.Index, *Info.Key.ToString(),
            Info.Inputs.Num(), Info.Outputs.Num());

#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
        for (int32 i = 0; i < Info.Inputs.Num(); ++i)
        {
            const FInoChatterboxLiteRTTensorMeta& M = Info.Inputs[i];
            FString Shape = TEXT("[");
            for (int32 d = 0; d < M.Dims.Num(); ++d)
            {
                if (d > 0) Shape += TEXT(", ");
                Shape += (M.Dims[d] < 0) ? FString(TEXT("?")) : FString::FromInt(M.Dims[d]);
            }
            Shape += TEXT("]");
            UE_LOG(LogInoChatterboxLiteRT, Log,
                TEXT("    in [%d] %-30s  %s %s"),
                i, *M.Name.ToString(),
                InoChatterboxLiteRT::ElementTypeName(M.ElementType),
                *Shape);
        }
        for (int32 i = 0; i < Info.Outputs.Num(); ++i)
        {
            const FInoChatterboxLiteRTTensorMeta& M = Info.Outputs[i];
            FString Shape = TEXT("[");
            for (int32 d = 0; d < M.Dims.Num(); ++d)
            {
                if (d > 0) Shape += TEXT(", ");
                Shape += (M.Dims[d] < 0) ? FString(TEXT("?")) : FString::FromInt(M.Dims[d]);
            }
            Shape += TEXT("]");
            UE_LOG(LogInoChatterboxLiteRT, Log,
                TEXT("    out[%d] %-30s  %s %s"),
                i, *M.Name.ToString(),
                InoChatterboxLiteRT::ElementTypeName(M.ElementType),
                *Shape);
        }
#endif
    }
}
