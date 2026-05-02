// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoQwen3ASRLiteRTTensor.h"

#include "InoQwen3ASRLiteRT.h"

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
#include "litert/c/litert_layout.h"
#include "litert/c/litert_tensor_buffer_types.h"
#endif

FInoQwen3ASRLiteRTTensor::FInoQwen3ASRLiteRTTensor(FInoQwen3ASRLiteRTTensor&& Other) noexcept
{
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    Handle = Other.Handle;
    Type = Other.Type;
    DimsCache = MoveTemp(Other.DimsCache);
    Other.Handle = nullptr;
    Other.Type = kLiteRtElementTypeNone;
#endif
}

FInoQwen3ASRLiteRTTensor& FInoQwen3ASRLiteRTTensor::operator=(FInoQwen3ASRLiteRTTensor&& Other) noexcept
{
    if (this == &Other) { return *this; }
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    if (Handle) { LiteRtDestroyTensorBuffer(Handle); }
    Handle = Other.Handle;
    Type = Other.Type;
    DimsCache = MoveTemp(Other.DimsCache);
    Other.Handle = nullptr;
    Other.Type = kLiteRtElementTypeNone;
#endif
    return *this;
}

FInoQwen3ASRLiteRTTensor::~FInoQwen3ASRLiteRTTensor()
{
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    if (Handle) { LiteRtDestroyTensorBuffer(Handle); Handle = nullptr; }
#endif
}

#if PLATFORM_WINDOWS || PLATFORM_ANDROID

bool FInoQwen3ASRLiteRTTensor::CreateManagedHost(
    LiteRtEnvironment Env,
    LiteRtElementType ElementType,
    TArrayView<const int32> Dims)
{
    if (Handle) { LiteRtDestroyTensorBuffer(Handle); Handle = nullptr; }
    Type = kLiteRtElementTypeNone;
    DimsCache.Reset();

    if (!Env)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error, TEXT("CreateManagedHost: null env."));
        return false;
    }
    if (Dims.Num() == 0 || Dims.Num() > LITERT_TENSOR_MAX_RANK)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("CreateManagedHost: rank %d out of range."), Dims.Num());
        return false;
    }
    const size_t ElemBytes = InoQwen3ASRLiteRT::ElementByteSize(ElementType);
    if (ElemBytes == 0)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("CreateManagedHost: unsupported element type %d."),
            static_cast<int32>(ElementType));
        return false;
    }

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
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("CreateManagedHost: dynamic dim %d not allowed."), Dims[i]);
            return false;
        }
        TotalElems *= static_cast<size_t>(Dims[i]);
    }
    const size_t ByteSize = TotalElems * ElemBytes;

    const LiteRtStatus Status = LiteRtCreateManagedTensorBuffer(
        Env, kLiteRtTensorBufferTypeHostMemory, &TensorType, ByteSize, &Handle);
    if (Status != kLiteRtStatusOk || !Handle)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("LiteRtCreateManagedTensorBuffer failed: %s, bytes=%llu."),
            *InoQwen3ASRLiteRT::StatusToString(Status),
            static_cast<uint64>(ByteSize));
        Handle = nullptr;
        return false;
    }

    Type = ElementType;
    DimsCache.Append(Dims.GetData(), Dims.Num());
    return true;
}

void* FInoQwen3ASRLiteRTTensor::LockForWrite()
{
    if (!Handle) { return nullptr; }
    void* HostPtr = nullptr;
    const LiteRtStatus Status = LiteRtLockTensorBuffer(
        Handle, &HostPtr, kLiteRtTensorBufferLockModeWrite);
    if (Status != kLiteRtStatusOk)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("LockForWrite failed: %s."),
            *InoQwen3ASRLiteRT::StatusToString(Status));
        return nullptr;
    }
    return HostPtr;
}

const void* FInoQwen3ASRLiteRTTensor::LockForRead()
{
    if (!Handle) { return nullptr; }
    void* HostPtr = nullptr;
    const LiteRtStatus Status = LiteRtLockTensorBuffer(
        Handle, &HostPtr, kLiteRtTensorBufferLockModeRead);
    if (Status != kLiteRtStatusOk)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Error,
            TEXT("LockForRead failed: %s."),
            *InoQwen3ASRLiteRT::StatusToString(Status));
        return nullptr;
    }
    return HostPtr;
}

void FInoQwen3ASRLiteRTTensor::Unlock()
{
    if (!Handle) { return; }
    const LiteRtStatus Status = LiteRtUnlockTensorBuffer(Handle);
    if (Status != kLiteRtStatusOk)
    {
        UE_LOG(LogInoQwen3ASRLiteRT, Warning,
            TEXT("Unlock failed: %s."),
            *InoQwen3ASRLiteRT::StatusToString(Status));
    }
}

size_t FInoQwen3ASRLiteRTTensor::PackedBytes() const
{
    if (!Handle) { return 0; }
    size_t Bytes = 0;
    if (LiteRtGetTensorBufferPackedSize(Handle, &Bytes) != kLiteRtStatusOk) { return 0; }
    return Bytes;
}

#endif  // PLATFORM_WINDOWS || PLATFORM_ANDROID

namespace InoQwen3ASRLiteRT
{
#if PLATFORM_WINDOWS || PLATFORM_ANDROID

    size_t ElementByteSize(LiteRtElementType Type)
    {
        switch (Type)
        {
        case kLiteRtElementTypeBool:
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
        if (Layout.rank == 0) { return TEXT("[]"); }
        FString Result = TEXT("[");
        for (unsigned int i = 0; i < Layout.rank; ++i)
        {
            if (i > 0) Result += TEXT(", ");
            const int32 Dim = Layout.dimensions[i];
            Result += (Dim < 0) ? FString(TEXT("?")) : FString::FromInt(Dim);
        }
        Result += TEXT("]");
        return Result;
    }

    FString StatusToString(LiteRtStatus Status)
    {
        const TCHAR* Name = nullptr;
        switch (Status)
        {
        case kLiteRtStatusOk:                                Name = TEXT("Ok"); break;
        case kLiteRtStatusErrorInvalidArgument:              Name = TEXT("ErrorInvalidArgument"); break;
        case kLiteRtStatusErrorMemoryAllocationFailure:      Name = TEXT("ErrorMemoryAllocationFailure"); break;
        case kLiteRtStatusErrorRuntimeFailure:               Name = TEXT("ErrorRuntimeFailure"); break;
        case kLiteRtStatusErrorMissingInputTensor:           Name = TEXT("ErrorMissingInputTensor"); break;
        case kLiteRtStatusErrorUnsupported:                  Name = TEXT("ErrorUnsupported"); break;
        case kLiteRtStatusErrorNotFound:                     Name = TEXT("ErrorNotFound"); break;
        case kLiteRtStatusErrorTimeoutExpired:               Name = TEXT("ErrorTimeoutExpired"); break;
        case kLiteRtStatusErrorWrongVersion:                 Name = TEXT("ErrorWrongVersion"); break;
        case kLiteRtStatusErrorUnknown:                      Name = TEXT("ErrorUnknown"); break;
        case kLiteRtStatusErrorAlreadyExists:                Name = TEXT("ErrorAlreadyExists"); break;
        case kLiteRtStatusCancelled:                         Name = TEXT("Cancelled"); break;
        case kLiteRtStatusErrorFileIO:                       Name = TEXT("ErrorFileIO"); break;
        case kLiteRtStatusErrorInvalidFlatbuffer:            Name = TEXT("ErrorInvalidFlatbuffer"); break;
        case kLiteRtStatusErrorDynamicLoading:               Name = TEXT("ErrorDynamicLoading"); break;
        case kLiteRtStatusErrorSerialization:                Name = TEXT("ErrorSerialization"); break;
        case kLiteRtStatusErrorCompilation:                  Name = TEXT("ErrorCompilation"); break;
        case kLiteRtStatusErrorIndexOOB:                     Name = TEXT("ErrorIndexOOB"); break;
        case kLiteRtStatusErrorInvalidIrType:                Name = TEXT("ErrorInvalidIrType"); break;
        case kLiteRtStatusErrorInvalidGraphInvariant:        Name = TEXT("ErrorInvalidGraphInvariant"); break;
        case kLiteRtStatusErrorGraphModification:            Name = TEXT("ErrorGraphModification"); break;
        case kLiteRtStatusErrorInvalidToolConfig:            Name = TEXT("ErrorInvalidToolConfig"); break;
        case kLiteRtStatusLegalizeNoMatch:                   Name = TEXT("LegalizeNoMatch"); break;
        case kLiteRtStatusErrorInvalidLegalization:          Name = TEXT("ErrorInvalidLegalization"); break;
        case kLiteRtStatusPatternNoMatch:                    Name = TEXT("PatternNoMatch"); break;
        case kLiteRtStatusInvalidTransformation:             Name = TEXT("InvalidTransformation"); break;
        case kLiteRtStatusErrorUnsupportedRuntimeVersion:    Name = TEXT("ErrorUnsupportedRuntimeVersion"); break;
        case kLiteRtStatusErrorUnsupportedCompilerVersion:   Name = TEXT("ErrorUnsupportedCompilerVersion"); break;
        case kLiteRtStatusErrorIncompatibleByteCodeVersion:  Name = TEXT("ErrorIncompatibleByteCodeVersion"); break;
        case kLiteRtStatusErrorUnsupportedOpShapeInferer:    Name = TEXT("ErrorUnsupportedOpShapeInferer"); break;
        case kLiteRtStatusErrorShapeInferenceFailed:         Name = TEXT("ErrorShapeInferenceFailed"); break;
        default: break;
        }
        if (Name)
        {
            return FString::Printf(TEXT("%s(%d)"), Name, static_cast<int32>(Status));
        }
        return FString::Printf(TEXT("status=%d"), static_cast<int32>(Status));
    }

#endif
}
