// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoQwen3ASRLiteRTEnv.h"

#include "InoQwen3ASRLiteRT.h"
#include "InoQwen3ASRLiteRTTensor.h"  // for InoQwen3ASRLiteRT::StatusToString

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
#include "litert/c/litert_common.h"
#include "litert/c/litert_environment.h"
#endif

namespace InoQwen3ASRLiteRT
{
    namespace
    {
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
        FCriticalSection GEnvLock;
        LiteRtEnvironment GEnv = nullptr;
        bool bEnvCreationAttempted = false;
#endif
    }

    LiteRtEnvironment GetEnvironment()
    {
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
        FScopeLock Lock(&GEnvLock);
        if (GEnv)
        {
            return GEnv;
        }
        if (bEnvCreationAttempted)
        {
            return nullptr;
        }
        bEnvCreationAttempted = true;

        const LiteRtStatus Status = LiteRtCreateEnvironment(
            /*num_options=*/0, /*options=*/nullptr, &GEnv);
        if (Status != kLiteRtStatusOk)
        {
            UE_LOG(LogInoQwen3ASRLiteRT, Error,
                TEXT("LiteRtCreateEnvironment failed: %s. Subsequent model "
                     "loads will return nullptr."),
                *InoQwen3ASRLiteRT::StatusToString(Status));
            GEnv = nullptr;
            return nullptr;
        }

        UE_LOG(LogInoQwen3ASRLiteRT, Log,
            TEXT("LiteRtEnvironment created (process singleton)."));
        return GEnv;
#else
        UE_LOG(LogInoQwen3ASRLiteRT, Warning,
            TEXT("LiteRtEnvironment unavailable on this platform."));
        return nullptr;
#endif
    }

    void Shutdown()
    {
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
        FScopeLock Lock(&GEnvLock);
        if (GEnv)
        {
            LiteRtDestroyEnvironment(GEnv);
            GEnv = nullptr;
            UE_LOG(LogInoQwen3ASRLiteRT, Log, TEXT("LiteRtEnvironment destroyed."));
        }
        bEnvCreationAttempted = false;
#endif
    }
}
