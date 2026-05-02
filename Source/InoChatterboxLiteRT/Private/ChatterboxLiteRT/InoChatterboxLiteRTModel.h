// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
#include "litert/c/litert_common.h"
#include "litert/c/litert_model_types.h"
#endif

struct FInoChatterboxLiteRTTensor;

/**
 * Per-tensor metadata cached at model load — name + element type + shape.
 * Shape entries < 0 indicate dynamic dimensions. Held in TArray for
 * positional + name lookups.
 */
struct FInoChatterboxLiteRTTensorMeta
{
    FName Name;
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    LiteRtElementType ElementType = kLiteRtElementTypeNone;
#endif
    TArray<int32, TInlineAllocator<4>> Dims;
};

/**
 * Per-signature cached lookup tables for inputs + outputs. Built once at
 * Load() time from the LiteRT model metadata so per-call inference doesn't
 * re-walk the signature lists.
 *
 * The order of `Inputs` matches the positional index expected by
 * LiteRtRunCompiledModel's `input_buffers[]` for this signature; `Outputs`
 * matches `output_buffers[]`. Names → positions are also keyed in the
 * companion TMaps for O(1) name-based dispatch.
 *
 * Different signatures of the same model (e.g. prefill_64 vs decode of
 * language_model.tflite) have INDEPENDENT input orderings — always look
 * up the index for the signature you're running, never assume cross-
 * signature alignment.
 */
struct FInoChatterboxLiteRTSignatureInfo
{
    FName Key;                                      // signature name (e.g. "prefill_64", "decode")
    int32 Index = INDEX_NONE;                       // positional index into the model's signature array
    TArray<FInoChatterboxLiteRTTensorMeta> Inputs;
    TArray<FInoChatterboxLiteRTTensorMeta> Outputs;
    TMap<FName, int32> InputIndexByName;
    TMap<FName, int32> OutputIndexByName;
};

/**
 * RAII wrapper around { LiteRtModel, LiteRtCompiledModel } for one .tflite
 * file. Holds cached signature + I/O metadata so per-call inference can
 * look up positional indices in O(1).
 *
 * Move-only.
 */
struct FInoChatterboxLiteRTModel
{
public:
    FInoChatterboxLiteRTModel() = default;
    FInoChatterboxLiteRTModel(const FInoChatterboxLiteRTModel&) = delete;
    FInoChatterboxLiteRTModel& operator=(const FInoChatterboxLiteRTModel&) = delete;
    FInoChatterboxLiteRTModel(FInoChatterboxLiteRTModel&& Other) noexcept;
    FInoChatterboxLiteRTModel& operator=(FInoChatterboxLiteRTModel&& Other) noexcept;
    ~FInoChatterboxLiteRTModel();

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    /**
     * Load a .tflite from disk and create a CompiledModel against the given
     * environment. `HardwareAcceleratorMask` is a bitmask of LiteRtHwAccelerators
     * (use `kLiteRtHwAcceleratorCpu` for CPU/XNNPack).
     *
     * Returns true on success. On failure, all internal state is cleared and
     * the error is logged.
     */
    bool Load(
        LiteRtEnvironment Env,
        const FString& AbsolutePath,
        int32 HardwareAcceleratorMask);

    /** Returns INDEX_NONE if the signature key is not found. */
    int32 GetSignatureIndex(FName SignatureKey) const;

    const FInoChatterboxLiteRTSignatureInfo* GetSignatureInfo(int32 SignatureIndex) const;
    const FInoChatterboxLiteRTSignatureInfo* GetSignatureInfo(FName SignatureKey) const;

    /** Returns INDEX_NONE if not found. */
    int32 GetInputIndex(int32 SignatureIndex, FName InputName) const;
    int32 GetOutputIndex(int32 SignatureIndex, FName OutputName) const;

    /**
     * Run inference. `Inputs` and `Outputs` must be sized + ordered to match
     * the signature's input/output positional indices (use GetInputIndex /
     * GetOutputIndex to slot specific named tensors into the right positions).
     *
     * Returns true on success.
     */
    bool Run(
        int32 SignatureIndex,
        TArrayView<FInoChatterboxLiteRTTensor*> Inputs,
        TArrayView<FInoChatterboxLiteRTTensor*> Outputs);

    /**
     * Resize an input tensor to a concrete shape (for models with dynamic
     * dimensions in the signature). REQUIRED before Run() if the input has
     * any dynamic dim — the LiteRT compiled-model runtime uses this to
     * allocate internal pipeline buffers and to infer the output shape.
     *
     * After calling this, you should:
     *   1. (Re)allocate input tensor buffers matching the new shape
     *   2. Call GetOutputTensorLayout() to learn the now-concrete output shape
     *   3. (Re)allocate output tensor buffers matching that shape
     *   4. Run()
     *
     * Returns true on success.
     */
    bool ResizeInputTensor(
        int32 SignatureIndex,
        int32 InputIndex,
        TArrayView<const int32> NewDims);

    /**
     * Get the current (post-resize) layout of an output tensor for the given
     * signature. Pass `bUpdateAllocation=true` after a fresh ResizeInputTensor
     * call so the runtime propagates the shape change before reporting.
     *
     * Returns true on success and fills `OutDims` with the concrete dimensions
     * (negative entries indicate any dimensions that are still dynamic; for
     * a properly-resized model these should all be > 0).
     */
    bool GetOutputTensorLayout(
        int32 SignatureIndex,
        int32 OutputIndex,
        TArray<int32>& OutDims,
        bool bUpdateAllocation = true);

    /**
     * Diagnostic: get the current input tensor layout (reflecting the most
     * recent ResizeInputTensor call, per the LiteRT C API docs). Useful for
     * verifying that resize calls are actually taking effect.
     */
    bool GetInputTensorLayout(
        int32 SignatureIndex,
        int32 InputIndex,
        TArray<int32>& OutDims);

    /** Number of signatures cached. */
    int32 NumSignatures() const { return Signatures.Num(); }

    const TArray<FInoChatterboxLiteRTSignatureInfo>& GetSignatures() const { return Signatures; }

    /** Path used at Load() — for logging. */
    const FString& GetPath() const { return ModelPath; }

    /** Probe via LiteRtCompiledModelIsFullyAccelerated. False if the model couldn't be loaded. */
    bool IsFullyAccelerated() const;
#endif

    /** Diagnostic dump of every signature's I/O to LogInoChatterboxLiteRT. */
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
    TArray<FInoChatterboxLiteRTSignatureInfo> Signatures;
    TMap<FName, int32> SignatureIndexByName;
};
