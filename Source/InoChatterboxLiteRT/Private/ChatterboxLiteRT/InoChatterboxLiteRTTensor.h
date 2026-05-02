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
 * - Move-only (raw LiteRtTensorBuffer handles must not be duplicated; the
 *   runtime ref-counts via LiteRtDuplicateTensorBuffer if you really want
 *   that, but we don't need it for the inference loop).
 * - Owns the LiteRtTensorBuffer; destructor calls LiteRtDestroyTensorBuffer.
 * - Provides typed Lock<T>() / Unlock() helpers for write- and read-side
 *   buffer access (mapped to LiteRtLockTensorBuffer with appropriate mode).
 *
 * For Phase 1 we exclusively use managed host-memory buffers via
 * LiteRtCreateManagedTensorBuffer. GPU-resident buffers + zero-copy paths
 * land in later phases (would be allocated with a different
 * kLiteRtTensorBufferType*).
 */
struct FInoChatterboxLiteRTTensor
{
public:
    FInoChatterboxLiteRTTensor() = default;

    // Move-only. LiteRtTensorBuffer is a raw handle; copying would alias
    // ownership and double-destroy.
    FInoChatterboxLiteRTTensor(const FInoChatterboxLiteRTTensor&) = delete;
    FInoChatterboxLiteRTTensor& operator=(const FInoChatterboxLiteRTTensor&) = delete;

    FInoChatterboxLiteRTTensor(FInoChatterboxLiteRTTensor&& Other) noexcept;
    FInoChatterboxLiteRTTensor& operator=(FInoChatterboxLiteRTTensor&& Other) noexcept;

    ~FInoChatterboxLiteRTTensor();

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    /**
     * Allocate a managed host-memory tensor of the given element type +
     * shape. Returns true on success, false on allocation / API failure
     * (logged at Error). The previously-held buffer (if any) is released.
     *
     * Caller MUST pass an env from InoChatterboxLiteRT::GetEnvironment()
     * so the buffer is bound to the same accelerator registry as the
     * CompiledModel that will consume it.
     */
    bool CreateManagedHost(
        LiteRtEnvironment Env,
        LiteRtElementType ElementType,
        TArrayView<const int32> Dims);

    /** Underlying handle. nullptr if unallocated. */
    LiteRtTensorBuffer Get() const { return Handle; }

    /**
     * Lock for host write — memcpy your data into HostPtr, then call Unlock.
     * Returns nullptr on failure.
     */
    void* LockForWrite();

    /**
     * Lock for host read — read from the returned ptr (size = PackedBytes()),
     * then call Unlock.
     */
    const void* LockForRead();

    void Unlock();

    /** Number of bytes in the packed (logical) layout. 0 if unallocated. */
    size_t PackedBytes() const;

    /** Element type passed at creation. */
    LiteRtElementType ElementType() const { return Type; }

    /** Shape captured at creation. */
    TArrayView<const int32> Dims() const { return DimsCache; }
#endif

private:
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    LiteRtTensorBuffer Handle = nullptr;
    LiteRtElementType Type = kLiteRtElementTypeNone;
    TArray<int32, TInlineAllocator<4>> DimsCache;
#endif
};

namespace InoChatterboxLiteRT
{
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    /** Returns the size in bytes of a single element of the given type. 0 if unknown. */
    size_t ElementByteSize(LiteRtElementType Type);

    /** Returns a short human-readable name like "f32", "i32", "f16". */
    const TCHAR* ElementTypeName(LiteRtElementType Type);

    /** Format a LiteRtLayout dimensions array as "[1, 24, 64]" (or "[1, ?, 1024]" for dynamic). */
    FString FormatLayout(const LiteRtLayout& Layout);

    /** Wrap a `LiteRtStatus` lookup so call sites can log without ANSI_TO_TCHAR each time. */
    FString StatusToString(LiteRtStatus Status);
#endif
}
