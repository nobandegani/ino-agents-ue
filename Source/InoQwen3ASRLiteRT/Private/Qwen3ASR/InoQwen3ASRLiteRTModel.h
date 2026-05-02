// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
#include "litert/c/litert_common.h"
#include "litert/c/litert_model_types.h"
#endif

struct FInoQwen3ASRLiteRTTensor;

struct FInoQwen3ASRLiteRTTensorMeta
{
    FName Name;
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    LiteRtElementType ElementType = kLiteRtElementTypeNone;
#endif
    TArray<int32, TInlineAllocator<4>> Dims;
};

struct FInoQwen3ASRLiteRTSignatureInfo
{
    FName Key;
    int32 Index = INDEX_NONE;
    TArray<FInoQwen3ASRLiteRTTensorMeta> Inputs;
    TArray<FInoQwen3ASRLiteRTTensorMeta> Outputs;
    TMap<FName, int32> InputIndexByName;
    TMap<FName, int32> OutputIndexByName;
};

/**
 * RAII wrapper around { LiteRtModel, LiteRtCompiledModel } for one .tflite
 * file.
 *
 * Caches per-signature input/output positional indices + tensor metadata at
 * Load() time so per-call inference can look up names → positions in O(1).
 * Different signatures (e.g. encoder vs decoder if the model exposes them
 * separately) get INDEPENDENT positional tables — never assume cross-
 * signature alignment.
 *
 * Move-only.
 */
struct FInoQwen3ASRLiteRTModel
{
public:
    FInoQwen3ASRLiteRTModel() = default;
    FInoQwen3ASRLiteRTModel(const FInoQwen3ASRLiteRTModel&) = delete;
    FInoQwen3ASRLiteRTModel& operator=(const FInoQwen3ASRLiteRTModel&) = delete;
    FInoQwen3ASRLiteRTModel(FInoQwen3ASRLiteRTModel&& Other) noexcept;
    FInoQwen3ASRLiteRTModel& operator=(FInoQwen3ASRLiteRTModel&& Other) noexcept;
    ~FInoQwen3ASRLiteRTModel();

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    bool Load(LiteRtEnvironment Env, const FString& AbsolutePath, int32 HardwareAcceleratorMask);

    int32 GetSignatureIndex(FName SignatureKey) const;
    const FInoQwen3ASRLiteRTSignatureInfo* GetSignatureInfo(int32 SignatureIndex) const;
    const FInoQwen3ASRLiteRTSignatureInfo* GetSignatureInfo(FName SignatureKey) const;

    int32 GetInputIndex(int32 SignatureIndex, FName InputName) const;
    int32 GetOutputIndex(int32 SignatureIndex, FName OutputName) const;

    bool Run(int32 SignatureIndex,
        TArrayView<FInoQwen3ASRLiteRTTensor*> Inputs,
        TArrayView<FInoQwen3ASRLiteRTTensor*> Outputs);

    bool ResizeInputTensor(int32 SignatureIndex, int32 InputIndex, TArrayView<const int32> NewDims);
    bool GetOutputTensorLayout(int32 SignatureIndex, int32 OutputIndex, TArray<int32>& OutDims, bool bUpdateAllocation = true);
    bool GetInputTensorLayout(int32 SignatureIndex, int32 InputIndex, TArray<int32>& OutDims);

    int32 NumSignatures() const { return Signatures.Num(); }
    const TArray<FInoQwen3ASRLiteRTSignatureInfo>& GetSignatures() const { return Signatures; }
    const FString& GetPath() const { return ModelPath; }
    bool IsFullyAccelerated() const;
#endif

    void LogSignatures() const;

private:
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    void Reset();
    bool BuildSignatureCache();
#endif

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    LiteRtModel Model = nullptr;
    LiteRtCompiledModel CompiledModel = nullptr;
#endif
    FString ModelPath;
    TArray<FInoQwen3ASRLiteRTSignatureInfo> Signatures;
    TMap<FName, int32> SignatureIndexByName;
};
