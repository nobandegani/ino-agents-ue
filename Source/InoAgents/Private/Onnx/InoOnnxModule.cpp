// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoOnnxModule.h"

#include "InoAgentsLog.h"

#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

// ONNX Runtime C API. Included for the struct / function-type definitions
// (OrtApi, OrtApiBase, OrtStatus, OrtGetApiBase signature, etc.). We do
// NOT link against the ORT import library:
//   Windows: we GetProcAddress "OrtGetApiBase" on the renamed
//            InoOnnxRuntime.dll at runtime.
//   Android: libUnreal.so's DT_NEEDED on libonnxruntime.so causes the
//            dynamic linker to resolve OrtGetApiBase for us at load
//            time, so a direct call from this TU is fine.
//
// We deliberately stay on the C API (not onnxruntime_cxx_api.h) so we
// remain exception-free (UE modules default bEnableExceptions=false).
#include "onnxruntime_c_api.h"

namespace InoAgents::Onnx
{

namespace
{
    /** Cached OrtApi vtable. Populated by Init(), cleared by Shutdown().
     *  Accessed via GetApi() from all ORT-consuming .cpps in the plugin. */
    const OrtApi* GOrtApi = nullptr;

#if PLATFORM_WINDOWS
    /** Compute the absolute path to our renamed ORT runtime DLL. */
    FString ResolveOnnxDllPath()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (!Plugin.IsValid())
        {
            return FString();
        }

        return FPaths::Combine(
            Plugin->GetBaseDir(),
            TEXT("Binaries/ThirdParty/InoOnnxRuntime/Win64"),
            TEXT("InoOnnxRuntime.dll"));
    }
#endif

    /**
     * Call OrtApi::GetAvailableProviders and log the returned provider
     * list. Runs once at Init() as proof the vtable is callable.
     */
    void LogAvailableProviders(const OrtApi* Api)
    {
        if (Api == nullptr)
        {
            return;
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
            return;
        }

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

        // ReleaseAvailableProviders is declared with warn_unused_result
        // (Android clang enforces this; MSVC is more forgiving). Capture
        // and release any returned OrtStatus. In practice this "free the
        // strings we just returned to you" call should never fail — but
        // ignoring a nodiscard return is a build-break on Android so we
        // handle it explicitly.
        if (OrtStatus* ReleaseStatus = Api->ReleaseAvailableProviders(ProvidersPtr, NumProviders))
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("InoAgents: OrtApi::ReleaseAvailableProviders returned an error (ignored): %s"),
                   UTF8_TO_TCHAR(Api->GetErrorMessage(ReleaseStatus)));
            Api->ReleaseStatus(ReleaseStatus);
        }
    }

    /**
     * Resolve OrtGetApiBase -> OrtApi* via the given API-base pointer.
     * Logs + returns nullptr on version mismatch.
     */
    const OrtApi* SelectOrtApi(const OrtApiBase* ApiBase)
    {
        if (ApiBase == nullptr)
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("InoAgents: OrtGetApiBase returned nullptr. The loaded ONNX Runtime is broken."));
            return nullptr;
        }

        const OrtApi* Api = ApiBase->GetApi(ORT_API_VERSION);
        if (Api == nullptr)
        {
            // This is what bit us on the original implicit-link attempt:
            // Windows was returning a handle to UE's bundled (older) ORT
            // because of LoadLibrary base-name caching, and that DLL did
            // not implement ORT_API_VERSION=24. With the InoOnnxRuntime.dll
            // rename we should never see this error — if it fires,
            // something is wrong with the staged binary or a stale copy
            // is lingering.
            UE_LOG(LogInoAgents, Error,
                   TEXT("InoAgents: OrtApiBase::GetApi(ORT_API_VERSION=%u) returned nullptr — ")
                   TEXT("the loaded ONNX Runtime does not implement this API version. ")
                   TEXT("Expected our pinned build (see Plugins/InoAgents/OnnxRuntime/ONNXRUNTIME_VERSION)."),
                   (uint32)ORT_API_VERSION);
            return nullptr;
        }

        return Api;
    }
}

void* Init()
{
#if PLATFORM_WINDOWS
    const FString Path = ResolveOnnxDllPath();
    if (Path.IsEmpty())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("InoAgents: could not resolve InoOnnxRuntime.dll path (plugin not found via IPluginManager?)."));
        return nullptr;
    }

    void* Handle = FPlatformProcess::GetDllHandle(*Path);
    if (Handle == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("InoAgents: failed to load InoOnnxRuntime.dll from %s. ")
               TEXT("Did you run Plugins/InoAgents/OnnxRuntime/scripts/setup-onnxruntime.ps1?"),
               *Path);
        return nullptr;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("InoAgents: loaded InoOnnxRuntime.dll from %s"),
           *Path);

    // Resolve the single entry-point symbol we need from our isolated
    // DLL. GetDllExport is UE's cross-platform wrapper over
    // GetProcAddress / dlsym; on Windows it's GetProcAddress here.
    using OrtGetApiBaseFn = const OrtApiBase* (*)();
    void* EntryPoint = FPlatformProcess::GetDllExport(Handle, TEXT("OrtGetApiBase"));
    if (EntryPoint == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("InoAgents: InoOnnxRuntime.dll does not export OrtGetApiBase. ")
               TEXT("The DLL is malformed or the rename step in setup-onnxruntime.ps1 picked up the wrong file."));
        FPlatformProcess::FreeDllHandle(Handle);
        return nullptr;
    }

    const OrtApiBase* ApiBase = reinterpret_cast<OrtGetApiBaseFn>(EntryPoint)();
    GOrtApi = SelectOrtApi(ApiBase);
    if (GOrtApi == nullptr)
    {
        FPlatformProcess::FreeDllHandle(Handle);
        return nullptr;
    }

    LogAvailableProviders(GOrtApi);
    return Handle;

#elif PLATFORM_ANDROID
    // On Android, libonnxruntime.so is resident by the time we get here:
    //   (a) It is DT_NEEDED by libUnreal.so (our Build.cs adds
    //       libonnxruntime.so via PublicAdditionalLibraries on the
    //       Android branch). Android's dynamic linker maps it
    //       recursively when libUnreal.so is loaded via
    //       System.loadLibrary("Unreal").
    //   (b) Our UPL XML additionally emits System.loadLibrary("onnxruntime")
    //       before that, as belt-and-suspenders.
    //
    // So OrtGetApiBase is a regular linker-resolved call from our TU —
    // no GetProcAddress/dlsym dance needed.
    const OrtApiBase* ApiBase = OrtGetApiBase();
    GOrtApi = SelectOrtApi(ApiBase);
    if (GOrtApi != nullptr)
    {
        LogAvailableProviders(GOrtApi);
    }
    return nullptr;

#else
    // iOS / Linux / macOS: InoOnnxRuntime.Build.cs has no platform branch
    // yet, so any Ort* call will fail to link before we even get here.
    UE_LOG(LogInoAgents, Warning,
           TEXT("InoAgents: ONNX Runtime is not yet available on this platform."));
    return nullptr;
#endif
}

void Shutdown(void* Handle)
{
    // Clear the cached OrtApi pointer first so any late callers of
    // GetApi() see nullptr rather than a vtable belonging to a DLL
    // we are about to unload. Happens-before ordering matters here.
    GOrtApi = nullptr;

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

const OrtApi* GetApi()
{
    return GOrtApi;
}

} // namespace InoAgents::Onnx
