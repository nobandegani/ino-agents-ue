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

    /**
     * Per-platform library name we feed to FPlatformProcess::GetDllHandle.
     *
     * Windows: our renamed DLL at an absolute path. We resolve the full
     *   path via IPluginManager so the loader can't be confused with any
     *   other onnxruntime.dll on the system.
     *
     * Android: bare soname. Android's dynamic linker resolves this via
     *   the APK's lib/<abi>/ dir (which is in LD_LIBRARY_PATH for the
     *   process). We can't build an absolute path because the APK's
     *   on-device lib dir ("/data/app/.../lib/arm64-v8a") isn't known
     *   at build time. The UPL's soLoadLibrary preload has already
     *   mapped the .so into the process by this point, so dlopen just
     *   returns the existing handle.
     */
    FString ResolveOnnxLibraryName()
    {
#if PLATFORM_WINDOWS
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return FPaths::Combine(
            Plugin->GetBaseDir(),
            TEXT("Binaries/ThirdParty/InoOnnxRuntime/Win64"),
            TEXT("InoOnnxRuntime.dll"));
#elif PLATFORM_ANDROID
        return FString(TEXT("libInoOnnxRuntime.so"));
#else
        return FString();
#endif
    }

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
#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    // Unified dlopen + dlsym path. We deliberately DO NOT link libUnreal
    // against our ORT .so on either platform — doing so on Android caused
    // clang's linker to resolve OrtGetApiBase against a marketplace
    // plugin's OLDER libonnxruntime.so (1.19.2) that was also on the
    // link path, recording the versioned symbol reference
    // OrtGetApiBase@VERS_1.19.2 in libUnreal.so. At runtime on device,
    // only OUR 1.24.3 .so (with OrtGetApiBase@@VERS_1.24.3) lives in the
    // APK, and the dynamic linker aborts the process when it can't find
    // the older version. By using GetDllExport("OrtGetApiBase") at
    // runtime we bypass the static linker entirely and bind to whatever
    // version our specific DLL/.so provides.
    const FString LibName = ResolveOnnxLibraryName();
    if (LibName.IsEmpty())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("InoAgents: could not resolve ONNX Runtime library name (IPluginManager failed?)."));
        return nullptr;
    }

    void* Handle = FPlatformProcess::GetDllHandle(*LibName);
    if (Handle == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("InoAgents: failed to load %s. ")
               TEXT("Did you run Plugins/InoAgents/OnnxRuntime/scripts/setup-onnxruntime.ps1 ")
               TEXT("and re-package?"),
               *LibName);
        return nullptr;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("InoAgents: loaded %s"),
           *LibName);

    // Resolve the single entry-point symbol we need. Everything else
    // goes through the OrtApi vtable returned by GetApiBase()->GetApi().
    // GetDllExport is UE's cross-platform wrapper over
    // GetProcAddress (Windows) / dlsym (Android).
    using OrtGetApiBaseFn = const OrtApiBase* (*)();
    void* EntryPoint = FPlatformProcess::GetDllExport(Handle, TEXT("OrtGetApiBase"));
    if (EntryPoint == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("InoAgents: %s does not export OrtGetApiBase. ")
               TEXT("The library is malformed or the setup script picked up the wrong file."),
               *LibName);
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

#else
    // iOS / Linux / macOS: InoOnnxRuntime.Build.cs has no platform branch
    // yet, so the .so/.dylib isn't staged. Any GetApi() caller will see
    // nullptr and handle it gracefully.
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

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    // We opened the handle via GetDllHandle (LoadLibrary / dlopen), so
    // we own a refcount and must release it here. On Android this
    // doesn't actually unmap the .so at process-shutdown time (the UPL
    // soLoadLibrary preload holds a separate refcount from the Java
    // side), but it keeps our bookkeeping clean and symmetric with
    // Windows, and avoids a leaked handle at editor-mode
    // module-unload/reload cycles.
    if (Handle != nullptr)
    {
        FPlatformProcess::FreeDllHandle(Handle);
    }
#else
    // iOS / Linux / macOS: nothing to free — Init() returned nullptr
    // before opening anything on those platforms.
    (void)Handle;
#endif
}

const OrtApi* GetApi()
{
    return GOrtApi;
}

} // namespace InoAgents::Onnx
