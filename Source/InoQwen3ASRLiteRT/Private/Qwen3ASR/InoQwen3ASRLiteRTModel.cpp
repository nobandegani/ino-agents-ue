// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoQwen3ASRLiteRTModel.h"

#include "InoQwen3ASRLiteRT.h"
#include "InoQwen3ASRLiteRTTensor.h"

#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
#include "litert/c/litert_compiled_model.h"
#include "litert/c/litert_environment.h"
#include "litert/c/litert_model.h"
#include "litert/c/litert_options.h"
#endif

FInoQwen3ASRLiteRTModel::FInoQwen3ASRLiteRTModel(FInoQwen3ASRLiteRTModel&& Other) noexcept
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

FInoQwen3ASRLiteRTModel& FInoQwen3ASRLiteRTModel::operator=(FInoQwen3ASRLiteRTModel&& Other) noexcept
{
    if (this == &Other) { return *this; }
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

FInoQwen3ASRLiteRTModel::~FInoQwen3ASRLiteRTModel()
{
#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
    Reset();
#endif
}

#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC

void FInoQwen3ASRLiteRTModel::Reset()
{
    if (CompiledModel) { LiteRtDestroyCompiledModel(CompiledModel); CompiledModel = nullptr; }
    if (Model) { LiteRtDestroyModel(Model); Model = nullptr; }
    Signatures.Reset();
    SignatureIndexByName.Reset();
    ModelPath.Reset();
}

bool FInoQwen3ASRLiteRTModel::Load(LiteRtEnvironment Env, const FString& AbsolutePath, int32 HardwareAcceleratorMask)
{
    Reset();
    if (!Env)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error, TEXT("Load(%s): null env."), *AbsolutePath);
        return false;
    }

    {
        FTCHARToUTF8 PathUtf8(*AbsolutePath);
        const LiteRtStatus Status = LiteRtCreateModelFromFile(PathUtf8.Get(), &Model);
        if (Status != kLiteRtStatusOk || !Model)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("LiteRtCreateModelFromFile('%s') failed: %s."),
                *AbsolutePath,
                *InoQwen3ASRLiteRT::StatusToString(Status));
            Model = nullptr;
            return false;
        }
    }

    LiteRtOptions Options = nullptr;
    if (LiteRtCreateOptions(&Options) != kLiteRtStatusOk || !Options)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error, TEXT("LiteRtCreateOptions failed."));
        Reset();
        return false;
    }
    LiteRtSetOptionsHardwareAccelerators(Options, static_cast<LiteRtHwAcceleratorSet>(HardwareAcceleratorMask));

    const LiteRtStatus Status = LiteRtCreateCompiledModel(Env, Model, Options, &CompiledModel);
    LiteRtDestroyOptions(Options);
    if (Status != kLiteRtStatusOk || !CompiledModel)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("LiteRtCreateCompiledModel('%s') failed: %s."),
            *AbsolutePath,
            *InoQwen3ASRLiteRT::StatusToString(Status));
        Reset();
        return false;
    }

    ModelPath = AbsolutePath;
    if (!BuildSignatureCache()) { Reset(); return false; }
    return true;
}

bool FInoQwen3ASRLiteRTModel::BuildSignatureCache()
{
    LiteRtParamIndex NumSigs = 0;
    if (LiteRtGetNumModelSignatures(Model, &NumSigs) != kLiteRtStatusOk) { return false; }
    if (NumSigs == 0)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error, TEXT("Model '%s' has 0 signatures."), *ModelPath);
        return false;
    }

    Signatures.Reserve(static_cast<int32>(NumSigs));
    SignatureIndexByName.Reserve(static_cast<int32>(NumSigs));

    for (LiteRtParamIndex SigIdx = 0; SigIdx < NumSigs; ++SigIdx)
    {
        LiteRtSignature Sig = nullptr;
        if (LiteRtGetModelSignature(Model, SigIdx, &Sig) != kLiteRtStatusOk || !Sig) { return false; }
        const char* KeyAnsi = nullptr;
        if (LiteRtGetSignatureKey(Sig, &KeyAnsi) != kLiteRtStatusOk || !KeyAnsi) { return false; }

        FInoQwen3ASRLiteRTSignatureInfo Info;
        Info.Index = static_cast<int32>(SigIdx);
        Info.Key = FName(ANSI_TO_TCHAR(KeyAnsi));

        LiteRtParamIndex NumInputs = 0;
        LiteRtGetNumSignatureInputs(Sig, &NumInputs);
        Info.Inputs.Reserve(static_cast<int32>(NumInputs));
        Info.InputIndexByName.Reserve(static_cast<int32>(NumInputs));
        for (LiteRtParamIndex InIdx = 0; InIdx < NumInputs; ++InIdx)
        {
            const char* NameAnsi = nullptr;
            if (LiteRtGetSignatureInputName(Sig, InIdx, &NameAnsi) != kLiteRtStatusOk || !NameAnsi) { return false; }
            FInoQwen3ASRLiteRTTensorMeta Meta;
            Meta.Name = FName(ANSI_TO_TCHAR(NameAnsi));
            LiteRtTensor T = nullptr;
            if (LiteRtGetSignatureInputTensorByIndex(Sig, InIdx, &T) == kLiteRtStatusOk && T)
            {
                LiteRtRankedTensorType R; FMemory::Memzero(R);
                if (LiteRtGetRankedTensorType(T, &R) == kLiteRtStatusOk)
                {
                    Meta.ElementType = R.element_type;
                    for (unsigned int d = 0; d < R.layout.rank; ++d) { Meta.Dims.Add(R.layout.dimensions[d]); }
                }
            }
            Info.InputIndexByName.Add(Meta.Name, static_cast<int32>(InIdx));
            Info.Inputs.Add(MoveTemp(Meta));
        }

        LiteRtParamIndex NumOutputs = 0;
        LiteRtGetNumSignatureOutputs(Sig, &NumOutputs);
        Info.Outputs.Reserve(static_cast<int32>(NumOutputs));
        Info.OutputIndexByName.Reserve(static_cast<int32>(NumOutputs));
        for (LiteRtParamIndex OutIdx = 0; OutIdx < NumOutputs; ++OutIdx)
        {
            const char* NameAnsi = nullptr;
            if (LiteRtGetSignatureOutputName(Sig, OutIdx, &NameAnsi) != kLiteRtStatusOk || !NameAnsi) { return false; }
            FInoQwen3ASRLiteRTTensorMeta Meta;
            Meta.Name = FName(ANSI_TO_TCHAR(NameAnsi));
            LiteRtTensor T = nullptr;
            if (LiteRtGetSignatureOutputTensorByIndex(Sig, OutIdx, &T) == kLiteRtStatusOk && T)
            {
                LiteRtRankedTensorType R; FMemory::Memzero(R);
                if (LiteRtGetRankedTensorType(T, &R) == kLiteRtStatusOk)
                {
                    Meta.ElementType = R.element_type;
                    for (unsigned int d = 0; d < R.layout.rank; ++d) { Meta.Dims.Add(R.layout.dimensions[d]); }
                }
            }
            Info.OutputIndexByName.Add(Meta.Name, static_cast<int32>(OutIdx));
            Info.Outputs.Add(MoveTemp(Meta));
        }

        SignatureIndexByName.Add(Info.Key, Info.Index);
        Signatures.Add(MoveTemp(Info));
    }
    return true;
}

int32 FInoQwen3ASRLiteRTModel::GetSignatureIndex(FName SignatureKey) const
{
    if (const int32* Found = SignatureIndexByName.Find(SignatureKey)) { return *Found; }
    return INDEX_NONE;
}

const FInoQwen3ASRLiteRTSignatureInfo* FInoQwen3ASRLiteRTModel::GetSignatureInfo(int32 SignatureIndex) const
{
    if (Signatures.IsValidIndex(SignatureIndex)) { return &Signatures[SignatureIndex]; }
    return nullptr;
}

const FInoQwen3ASRLiteRTSignatureInfo* FInoQwen3ASRLiteRTModel::GetSignatureInfo(FName SignatureKey) const
{
    return GetSignatureInfo(GetSignatureIndex(SignatureKey));
}

int32 FInoQwen3ASRLiteRTModel::GetInputIndex(int32 SignatureIndex, FName InputName) const
{
    if (const FInoQwen3ASRLiteRTSignatureInfo* Info = GetSignatureInfo(SignatureIndex))
    {
        if (const int32* Found = Info->InputIndexByName.Find(InputName)) { return *Found; }
    }
    return INDEX_NONE;
}

int32 FInoQwen3ASRLiteRTModel::GetOutputIndex(int32 SignatureIndex, FName OutputName) const
{
    if (const FInoQwen3ASRLiteRTSignatureInfo* Info = GetSignatureInfo(SignatureIndex))
    {
        if (const int32* Found = Info->OutputIndexByName.Find(OutputName)) { return *Found; }
    }
    return INDEX_NONE;
}

bool FInoQwen3ASRLiteRTModel::Run(
    int32 SignatureIndex,
    TArrayView<FInoQwen3ASRLiteRTTensor*> Inputs,
    TArrayView<FInoQwen3ASRLiteRTTensor*> Outputs)
{
    if (!CompiledModel)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error, TEXT("Run: model not loaded."));
        return false;
    }
    const FInoQwen3ASRLiteRTSignatureInfo* Info = GetSignatureInfo(SignatureIndex);
    if (!Info)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error, TEXT("Run: invalid signature index %d."), SignatureIndex);
        return false;
    }
    if (Inputs.Num() != Info->Inputs.Num() || Outputs.Num() != Info->Outputs.Num())
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("Run(%s): I/O count mismatch (in=%d/%d, out=%d/%d)."),
            *Info->Key.ToString(),
            Inputs.Num(), Info->Inputs.Num(),
            Outputs.Num(), Info->Outputs.Num());
        return false;
    }

    TArray<LiteRtTensorBuffer, TInlineAllocator<8>> InHandles;
    InHandles.Reserve(Inputs.Num());
    for (int32 i = 0; i < Inputs.Num(); ++i)
    {
        if (!Inputs[i] || !Inputs[i]->Get())
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("Run(%s): input %d (%s) null."),
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
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("Run(%s): output %d (%s) null."),
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
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("LiteRtRunCompiledModel(%s) failed: %s."),
            *Info->Key.ToString(),
            *InoQwen3ASRLiteRT::StatusToString(Status));
        return false;
    }
    return true;
}

bool FInoQwen3ASRLiteRTModel::IsFullyAccelerated() const
{
    if (!CompiledModel) { return false; }
    bool bAccel = false;
    if (LiteRtCompiledModelIsFullyAccelerated(CompiledModel, &bAccel) != kLiteRtStatusOk) { return false; }
    return bAccel;
}

bool FInoQwen3ASRLiteRTModel::ResizeInputTensor(int32 SignatureIndex, int32 InputIndex, TArrayView<const int32> NewDims)
{
    if (!CompiledModel) { return false; }
    if (NewDims.Num() == 0) { return false; }
    const LiteRtStatus Status = LiteRtCompiledModelResizeInputTensorNonStrict(
        CompiledModel,
        static_cast<LiteRtParamIndex>(SignatureIndex),
        static_cast<LiteRtParamIndex>(InputIndex),
        NewDims.GetData(),
        static_cast<size_t>(NewDims.Num()));
    if (Status != kLiteRtStatusOk)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("ResizeInputTensorNonStrict(sig=%d, in=%d) failed: %s."),
            SignatureIndex, InputIndex,
            *InoQwen3ASRLiteRT::StatusToString(Status));
        return false;
    }
    return true;
}

bool FInoQwen3ASRLiteRTModel::GetOutputTensorLayout(int32 SignatureIndex, int32 OutputIndex, TArray<int32>& OutDims, bool bUpdateAllocation)
{
    OutDims.Reset();
    if (!CompiledModel) { return false; }
    const FInoQwen3ASRLiteRTSignatureInfo* Info = GetSignatureInfo(SignatureIndex);
    if (!Info || !Info->Outputs.IsValidIndex(OutputIndex)) { return false; }

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
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("GetOutputTensorLayouts(sig=%d) failed: %s."),
            SignatureIndex,
            *InoQwen3ASRLiteRT::StatusToString(Status));
        return false;
    }
    const LiteRtLayout& L = Layouts[OutputIndex];
    OutDims.Reserve(L.rank);
    for (unsigned int i = 0; i < L.rank; ++i) { OutDims.Add(L.dimensions[i]); }
    return true;
}

bool FInoQwen3ASRLiteRTModel::GetInputTensorLayout(int32 SignatureIndex, int32 InputIndex, TArray<int32>& OutDims)
{
    OutDims.Reset();
    if (!CompiledModel) { return false; }
    LiteRtLayout L; FMemory::Memzero(L);
    const LiteRtStatus Status = LiteRtGetCompiledModelInputTensorLayout(
        CompiledModel,
        static_cast<LiteRtParamIndex>(SignatureIndex),
        static_cast<LiteRtParamIndex>(InputIndex),
        &L);
    if (Status != kLiteRtStatusOk) { return false; }
    OutDims.Reserve(L.rank);
    for (unsigned int i = 0; i < L.rank; ++i) { OutDims.Add(L.dimensions[i]); }
    return true;
}

#endif  // PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC

void FInoQwen3ASRLiteRTModel::LogSignatures() const
{
    UE_LOG(LogInoQwen3ASRLiteRT, Log,
        TEXT("--- Model: %s (signatures=%d) ---"), *ModelPath, Signatures.Num());
    for (const FInoQwen3ASRLiteRTSignatureInfo& Info : Signatures)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("  signature[%d] '%s'  inputs=%d outputs=%d"),
            Info.Index, *Info.Key.ToString(),
            Info.Inputs.Num(), Info.Outputs.Num());
#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_IOS || PLATFORM_MAC
        for (int32 i = 0; i < Info.Inputs.Num(); ++i)
        {
            const FInoQwen3ASRLiteRTTensorMeta& M = Info.Inputs[i];
            FString Shape = TEXT("[");
            for (int32 d = 0; d < M.Dims.Num(); ++d)
            {
                if (d > 0) Shape += TEXT(", ");
                Shape += (M.Dims[d] < 0) ? FString(TEXT("?")) : FString::FromInt(M.Dims[d]);
            }
            Shape += TEXT("]");
            UE_LOG(LogInoQwen3ASRLiteRT, Log,
                TEXT("    in [%d] %-30s  %s %s"),
                i, *M.Name.ToString(),
                InoQwen3ASRLiteRT::ElementTypeName(M.ElementType),
                *Shape);
        }
        for (int32 i = 0; i < Info.Outputs.Num(); ++i)
        {
            const FInoQwen3ASRLiteRTTensorMeta& M = Info.Outputs[i];
            FString Shape = TEXT("[");
            for (int32 d = 0; d < M.Dims.Num(); ++d)
            {
                if (d > 0) Shape += TEXT(", ");
                Shape += (M.Dims[d] < 0) ? FString(TEXT("?")) : FString::FromInt(M.Dims[d]);
            }
            Shape += TEXT("]");
            UE_LOG(LogInoQwen3ASRLiteRT, Log,
                TEXT("    out[%d] %-30s  %s %s"),
                i, *M.Name.ToString(),
                InoQwen3ASRLiteRT::ElementTypeName(M.ElementType),
                *Shape);
        }
#endif
    }
}
