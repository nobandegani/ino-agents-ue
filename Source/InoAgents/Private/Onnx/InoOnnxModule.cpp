// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoOnnxModule.h"

#include "InoAgentsLog.h"

#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

// ONNX Runtime C API. We deliberately use the C API (not onnxruntime_cxx_api.h)
// for module-startup code so we can stay exception-free — UE modules default
// to exceptions-off, and the C++ wrapper's Ort::GetAvailableProviders()
// throws Ort::Exception on failure. The C API returns OrtStatus* error
// handles instead, which integrate cleanly with our UE_LOG flow.
//
// Later phases (FInoOnnxSession and model-specific consumers) can revisit
// whether to flip bEnableExceptions=true for this module and switch to the
// C++ API; that's a scope-independent decision.
#include "onnxruntime_c_api.h"

namespace InoAgents::Onnx
{

namespace
{
    /**
     * Compute the absolute path to the staged onnxruntime.dll on Windows.
     * Returns empty on non-Windows platforms — callers are expected to
     * short-circuit on that.
     */
    FString ResolveOnnxDllPath()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (!Plugin.IsValid())
        {
            return FString();
        }

        const FString BaseDir = Plugin->GetBaseDir();

#if PLATFORM_WINDOWS
        return FPaths::Combine(
            BaseDir,
            TEXT("Binaries/ThirdParty/InoOnnxRuntime/Win64"),
            TEXT("onnxruntime.dll"));
#else
        return FString();
#endif
    }

    /**
     * Call OrtApi::GetAvailableProviders via the C API, log the returned
     * provider names to LogInoAgents at Log level, and release the
     * allocation. Exception-free. Returns true on success, false on error
     * (error is already logged).
     *
     * This is the equivalent of the litert_lm_set_min_log_level(0) smoke
     * test we do for LiteRT-LM at startup — a cheap, safe call that proves
     * the runtime is loaded and its symbols are callable.
     */
    bool RunProviderSmokeTest()
    {
        const OrtApiBase* ApiBase = OrtGetApiBase();
        if (ApiBase == nullptr)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("InoAgents: OrtGetApiBase() returned nullptr — ONNX Runtime is not functioning."));
            return false;
        }

        const OrtApi* Api = ApiBase->GetApi(ORT_API_VERSION);
        if (Api == nullptr)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("InoAgents: OrtApiBase::GetApi(ORT_API_VERSION=%u) returned nullptr — ")
                   TEXT("the linked onnxruntime.dll does not implement this API version."),
                   (uint32)ORT_API_VERSION);
            return false;
        }

        char** ProvidersPtr = nullptr;
        int    NumProviders = 0;
        OrtStatus* Status = Api->GetAvailableProviders(&ProvidersPtr, &NumProviders);

        if (Status != nullptr)
        {
            const char* ErrMsg = Api->GetErrorMessage(Status);
            UE_LOG(LogInoAgents, Error,
                   TEXT("InoAgents: OrtApi::GetAvailableProviders failed: %s"),
                   UTF8_TO_TCHAR(ErrMsg));
            Api->ReleaseStatus(Status);
            return false;
        }

        // Join the provider names into a single string for readability.
        FString Joined;
        for (int i = 0; i < NumProviders; ++i)
        {
            if (!Joined.IsEmpty())
            {
                Joined += TEXT(", ");
            }
            if (ProvidersPtr != nullptr && ProvidersPtr[i] != nullptr)
            {
                Joined += FString(UTF8_TO_TCHAR(ProvidersPtr[i]));
            }
        }

        UE_LOG(LogInoAgents, Log,
               TEXT("InoAgents: ONNX Runtime available providers: %s"),
               Joined.IsEmpty() ? TEXT("(none)") : *Joined);

        // ReleaseAvailableProviders is the dedicated deallocator — do NOT
        // call free/delete on ProvidersPtr directly.
        Api->ReleaseAvailableProviders(ProvidersPtr, NumProviders);

        return true;
    }
}

void* Init()
{
#if PLATFORM_WINDOWS
    const FString Path = ResolveOnnxDllPath();
    if (Path.IsEmpty())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("InoAgents: could not resolve onnxruntime.dll path (plugin not found via IPluginManager?)."));
        return nullptr;
    }

    void* Handle = FPlatformProcess::GetDllHandle(*Path);
    if (Handle == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("InoAgents: failed to load onnxruntime.dll from %s. ")
               TEXT("Did you run Plugins/InoAgents/OnnxRuntime/scripts/setup-onnxruntime.ps1?"),
               *Path);
        return nullptr;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("InoAgents: loaded onnxruntime.dll from %s"),
           *Path);

    RunProviderSmokeTest();
    return Handle;

#elif PLATFORM_ANDROID
    // On Android, libonnxruntime.so is resident before StartupModule runs:
    //   (a) It is DT_NEEDED by libUnreal.so (our Build.cs adds
    //       libonnxruntime.so via PublicAdditionalLibraries). Android's
    //       dynamic linker maps it recursively when libUnreal.so is
    //       loaded via System.loadLibrary("Unreal").
    //   (b) Our UPL XML additionally emits System.loadLibrary("onnxruntime")
    //       before that, as belt-and-suspenders.
    // Either way, by the time StartupModule runs we can call into the C
    // API directly. No FPlatformProcess::GetDllHandle needed.
    RunProviderSmokeTest();
    return nullptr;

#else
    // iOS / Linux / macOS: InoOnnxRuntime.Build.cs has no platform branch
    // yet, so any Ort* call will fail to link before we even get here.
    // If you're reading this because you're porting to a new platform,
    // extend InoOnnxRuntime.Build.cs first (and update
    // OnnxRuntime/scripts/setup-onnxruntime.ps1 to download the matching
    // prebuilt).
    UE_LOG(LogInoAgents, Warning,
           TEXT("InoAgents: ONNX Runtime is not yet available on this platform."));
    return nullptr;
#endif
}

void Shutdown(void* Handle)
{
#if PLATFORM_WINDOWS
    if (Handle != nullptr)
    {
        FPlatformProcess::FreeDllHandle(Handle);
    }
#else
    // No-op on Android / iOS / Linux / macOS: the shared library is
    // managed by the OS dynamic linker and freed at process exit along
    // with the rest of the game process.
    (void)Handle;
#endif
}

} // namespace InoAgents::Onnx
