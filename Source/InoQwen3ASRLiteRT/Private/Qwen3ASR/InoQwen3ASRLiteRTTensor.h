// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
#include "litert/c/litert_common.h"
#include "litert/c/litert_model_types.h"
#include "litert/c/litert_tensor_buffer.h"
#endif

/**
 * RAII wrapper around LiteRtTensorBuffer for host-memory-backed tensors.
 *
 * - Move-only (raw LiteRtTensorBuffer handles must not be aliased)
 * - Owns the LiteRtTensorBuffer; destructor calls LiteRtDestroyTensorBuffer
 * - Lock<T>() / Unlock() helpers for write- and read-side host access
 *
 * Phase 1 uses managed host-memory buffers (kLiteRtTensorBufferTypeHostMemory)
 * exclusively. GPU-resident buffers + zero-copy paths land in later phases
 * if we add a GPU accelerator opt-in.
 */
struct FInoQwen3ASRLiteRTTensor
{
public:
    FInoQwen3ASRLiteRTTensor() = default;

    FInoQwen3ASRLiteRTTensor(const FInoQwen3ASRLiteRTTensor&) = delete;
    FInoQwen3ASRLiteRTTensor& operator=(const FInoQwen3ASRLiteRTTensor&) = delete;

    FInoQwen3ASRLiteRTTensor(FInoQwen3ASRLiteRTTensor&& Other) noexcept;
    FInoQwen3ASRLiteRTTensor& operator=(FInoQwen3ASRLiteRTTensor&& Other) noexcept;

    ~FInoQwen3ASRLiteRTTensor();

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    bool CreateManagedHost(
        LiteRtEnvironment Env,
        LiteRtElementType ElementType,
        TArrayView<const int32> Dims);

    LiteRtTensorBuffer Get() const { return Handle; }

    void* LockForWrite();
    const void* LockForRead();
    void Unlock();

    size_t PackedBytes() const;

    LiteRtElementType ElementType() const { return Type; }
    TArrayView<const int32> Dims() const { return DimsCache; }
#endif

private:
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    LiteRtTensorBuffer Handle = nullptr;
    LiteRtElementType Type = kLiteRtElementTypeNone;
    TArray<int32, TInlineAllocator<4>> DimsCache;
#endif
};

namespace InoQwen3ASRLiteRT
{
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    size_t ElementByteSize(LiteRtElementType Type);
    const TCHAR* ElementTypeName(LiteRtElementType Type);
    FString FormatLayout(const LiteRtLayout& Layout);
    FString StatusToString(LiteRtStatus Status);
#endif
}
