// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxLiteRTEnv.h"

#include "InoChatterboxLiteRT.h"
#include "InoChatterboxLiteRTTensor.h"  // for InoChatterboxLiteRT::StatusToString

// LiteRT C API — staged into Source/ThirdParty/Public/litert/c/ by the
// sibling InoLiteRT plugin's build scripts. PublicSystemIncludePaths in
// InoLiteRT.Build.cs makes these resolve via <litert/...>.
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
#include "litert/c/litert_common.h"
#include "litert/c/litert_environment.h"
#endif

namespace InoChatterboxLiteRT
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
            // We already tried and failed; don't spam the log.
            return nullptr;
        }
        bEnvCreationAttempted = true;

        // Phase 1: no options — use the default accelerator registry. CPU
        // (XNNPack) is auto-applied to every CompiledModel; GPU registration
        // is opt-in per-model via LiteRtOptions in later phases.
        const LiteRtStatus Status = LiteRtCreateEnvironment(
            /*num_options=*/0, /*options=*/nullptr, &GEnv);
        if (Status != kLiteRtStatusOk)
        {
            UE_LOG(LogInoChatterboxLiteRT, Error,
                TEXT("LiteRtCreateEnvironment failed: %s. Subsequent model "
                     "loads will return nullptr."),
                *InoChatterboxLiteRT::StatusToString(Status));
            GEnv = nullptr;
            return nullptr;
        }

        UE_LOG(LogInoChatterboxLiteRT, Log,
            TEXT("LiteRtEnvironment created (process singleton)."));
        return GEnv;
#else
        // iOS / Linux / macOS: no LiteRT runtime is shipped by InoLiteRT yet.
        UE_LOG(LogInoChatterboxLiteRT, Warning,
            TEXT("LiteRtEnvironment unavailable on this platform — InoLiteRT "
                 "ships only Win64 + Android."));
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
            UE_LOG(LogInoChatterboxLiteRT, Log,
                TEXT("LiteRtEnvironment destroyed."));
        }
        bEnvCreationAttempted = false;
#endif
    }
}
