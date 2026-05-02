// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxLiteRTTensor.h"

#include "InoChatterboxLiteRT.h"

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
#include "litert/c/litert_layout.h"
#include "litert/c/litert_tensor_buffer_types.h"
#endif

FInoChatterboxLiteRTTensor::FInoChatterboxLiteRTTensor(FInoChatterboxLiteRTTensor&& Other) noexcept
{
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    Handle = Other.Handle;
    Type = Other.Type;
    DimsCache = MoveTemp(Other.DimsCache);
    Other.Handle = nullptr;
    Other.Type = kLiteRtElementTypeNone;
#endif
}

FInoChatterboxLiteRTTensor& FInoChatterboxLiteRTTensor::operator=(FInoChatterboxLiteRTTensor&& Other) noexcept
{
    if (this == &Other)
    {
        return *this;
    }
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    if (Handle)
    {
        LiteRtDestroyTensorBuffer(Handle);
    }
    Handle = Other.Handle;
    Type = Other.Type;
    DimsCache = MoveTemp(Other.DimsCache);
    Other.Handle = nullptr;
    Other.Type = kLiteRtElementTypeNone;
#endif
    return *this;
}

FInoChatterboxLiteRTTensor::~FInoChatterboxLiteRTTensor()
{
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    if (Handle)
    {
        LiteRtDestroyTensorBuffer(Handle);
        Handle = nullptr;
    }
#endif
}

#if PLATFORM_WINDOWS || PLATFORM_ANDROID

bool FInoChatterboxLiteRTTensor::CreateManagedHost(
    LiteRtEnvironment Env,
    LiteRtElementType ElementType,
    TArrayView<const int32> Dims)
{
    if (Handle)
    {
        LiteRtDestroyTensorBuffer(Handle);
        Handle = nullptr;
    }
    Type = kLiteRtElementTypeNone;
    DimsCache.Reset();

    if (!Env)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("CreateManagedHost: null environment."));
        return false;
    }
    if (Dims.Num() == 0 || Dims.Num() > LITERT_TENSOR_MAX_RANK)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("CreateManagedHost: rank %d out of range (max %d)."),
            Dims.Num(), LITERT_TENSOR_MAX_RANK);
        return false;
    }
    const size_t ElemBytes = InoChatterboxLiteRT::ElementByteSize(ElementType);
    if (ElemBytes == 0)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("CreateManagedHost: unsupported element type %d."),
            static_cast<int32>(ElementType));
        return false;
    }

    // Build LiteRtRankedTensorType.
    LiteRtRankedTensorType TensorType;
    FMemory::Memzero(TensorType);
    TensorType.element_type = ElementType;
    TensorType.layout.rank = static_cast<unsigned int>(Dims.Num());
    TensorType.layout.has_strides = false;
    size_t TotalElems = 1;
    for (int32 i = 0; i < Dims.Num(); ++i)
    {
        TensorType.layout.dimensions[i] = Dims[i];
        if (Dims[i] <= 0)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("CreateManagedHost: dynamic dim %d not allowed for managed alloc."),
                Dims[i]);
            return false;
        }
        TotalElems *= static_cast<size_t>(Dims[i]);
    }
    const size_t ByteSize = TotalElems * ElemBytes;

    const LiteRtStatus Status = LiteRtCreateManagedTensorBuffer(
        Env,
        kLiteRtTensorBufferTypeHostMemory,
        &TensorType,
        ByteSize,
        &Handle);
    if (Status != kLiteRtStatusOk || !Handle)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("LiteRtCreateManagedTensorBuffer failed: status=%d (%s), bytes=%llu."),
            static_cast<int32>(Status),
            *InoChatterboxLiteRT::StatusToString(Status),
            static_cast<uint64>(ByteSize));
        Handle = nullptr;
        return false;
    }

    Type = ElementType;
    DimsCache.Append(Dims.GetData(), Dims.Num());
    return true;
}

void* FInoChatterboxLiteRTTensor::LockForWrite()
{
    if (!Handle)
    {
        return nullptr;
    }
    void* HostPtr = nullptr;
    const LiteRtStatus Status = LiteRtLockTensorBuffer(
        Handle, &HostPtr, kLiteRtTensorBufferLockModeWrite);
    if (Status != kLiteRtStatusOk)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("LiteRtLockTensorBuffer(Write) failed: status=%d (%s)."),
            static_cast<int32>(Status),
            *InoChatterboxLiteRT::StatusToString(Status));
        return nullptr;
    }
    return HostPtr;
}

const void* FInoChatterboxLiteRTTensor::LockForRead()
{
    if (!Handle)
    {
        return nullptr;
    }
    void* HostPtr = nullptr;
    const LiteRtStatus Status = LiteRtLockTensorBuffer(
        Handle, &HostPtr, kLiteRtTensorBufferLockModeRead);
    if (Status != kLiteRtStatusOk)
    {
        UE_LOG(LogInoChatterboxLiteRT, Error,
            TEXT("LiteRtLockTensorBuffer(Read) failed: status=%d (%s)."),
            static_cast<int32>(Status),
            *InoChatterboxLiteRT::StatusToString(Status));
        return nullptr;
    }
    return HostPtr;
}

void FInoChatterboxLiteRTTensor::Unlock()
{
    if (!Handle)
    {
        return;
    }
    const LiteRtStatus Status = LiteRtUnlockTensorBuffer(Handle);
    if (Status != kLiteRtStatusOk)
    {
        UE_LOG(LogInoChatterboxLiteRT, Warning,
            TEXT("LiteRtUnlockTensorBuffer failed: status=%d (%s)."),
            static_cast<int32>(Status),
            *InoChatterboxLiteRT::StatusToString(Status));
    }
}

size_t FInoChatterboxLiteRTTensor::PackedBytes() const
{
    if (!Handle)
    {
        return 0;
    }
    size_t Bytes = 0;
    if (LiteRtGetTensorBufferPackedSize(Handle, &Bytes) != kLiteRtStatusOk)
    {
        return 0;
    }
    return Bytes;
}

#endif  // PLATFORM_WINDOWS || PLATFORM_ANDROID

namespace InoChatterboxLiteRT
{
#if PLATFORM_WINDOWS || PLATFORM_ANDROID

    size_t ElementByteSize(LiteRtElementType Type)
    {
        switch (Type)
        {
        case kLiteRtElementTypeBool:    return 1;
        case kLiteRtElementTypeInt8:
        case kLiteRtElementTypeUInt8:   return 1;
        case kLiteRtElementTypeInt16:
        case kLiteRtElementTypeUInt16:
        case kLiteRtElementTypeFloat16:
        case kLiteRtElementTypeBFloat16: return 2;
        case kLiteRtElementTypeInt32:
        case kLiteRtElementTypeUInt32:
        case kLiteRtElementTypeFloat32:  return 4;
        case kLiteRtElementTypeInt64:
        case kLiteRtElementTypeUInt64:
        case kLiteRtElementTypeFloat64:
        case kLiteRtElementTypeComplex64: return 8;
        case kLiteRtElementTypeComplex128: return 16;
        default: return 0;
        }
    }

    const TCHAR* ElementTypeName(LiteRtElementType Type)
    {
        switch (Type)
        {
        case kLiteRtElementTypeBool:       return TEXT("bool");
        case kLiteRtElementTypeInt8:       return TEXT("i8");
        case kLiteRtElementTypeInt16:      return TEXT("i16");
        case kLiteRtElementTypeInt32:      return TEXT("i32");
        case kLiteRtElementTypeInt64:      return TEXT("i64");
        case kLiteRtElementTypeUInt8:      return TEXT("u8");
        case kLiteRtElementTypeUInt16:     return TEXT("u16");
        case kLiteRtElementTypeUInt32:     return TEXT("u32");
        case kLiteRtElementTypeUInt64:     return TEXT("u64");
        case kLiteRtElementTypeFloat16:    return TEXT("f16");
        case kLiteRtElementTypeBFloat16:   return TEXT("bf16");
        case kLiteRtElementTypeFloat32:    return TEXT("f32");
        case kLiteRtElementTypeFloat64:    return TEXT("f64");
        case kLiteRtElementTypeComplex64:  return TEXT("c64");
        case kLiteRtElementTypeComplex128: return TEXT("c128");
        case kLiteRtElementTypeInt2:       return TEXT("i2");
        case kLiteRtElementTypeInt4:       return TEXT("i4");
        default: return TEXT("?");
        }
    }

    FString FormatLayout(const LiteRtLayout& Layout)
    {
        if (Layout.rank == 0)
        {
            return TEXT("[]");
        }
        FString Result = TEXT("[");
        for (unsigned int i = 0; i < Layout.rank; ++i)
        {
            if (i > 0) Result += TEXT(", ");
            const int32 Dim = Layout.dimensions[i];
            if (Dim < 0)
            {
                Result += TEXT("?");
            }
            else
            {
                Result += FString::FromInt(Dim);
            }
        }
        Result += TEXT("]");
        return Result;
    }

    FString StatusToString(LiteRtStatus Status)
    {
        const char* Msg = LiteRtGetStatusString(Status);
        return Msg ? FString(ANSI_TO_TCHAR(Msg)) : FString::Printf(TEXT("status=%d"), static_cast<int32>(Status));
    }

#endif  // PLATFORM_WINDOWS || PLATFORM_ANDROID
}
