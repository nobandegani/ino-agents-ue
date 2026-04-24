// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoOnnxModule.h"
#include "InoOnnxInternal.h"

#include "InoAgentsLog.h"

#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
    // For GetModuleHandleW / GetModuleFileNameW — used to verify which
    // DLL Windows' base-name cache actually served when we asked to
    // load a full-path DLL. Without this we can't distinguish "our
    // InoOnnxRuntime.dll / DirectML.dll / etc. got loaded" from "UE's
    // already-cached copy at a different path was returned instead."
    #include "Windows/AllowWindowsPlatformTypes.h"
    #include <windows.h>
    #include "Windows/HideWindowsPlatformTypes.h"
#endif

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

#if PLATFORM_WINDOWS
    /**
     * Windows-only: query Windows for the full on-disk path of an
     * already-loaded DLL (by its base name). Returns empty if the DLL
     * isn't loaded at all, or a sentinel string on API failure.
     *
     * Critical for verifying that our preloaded copies actually won
     * the base-name cache race vs other DLLs with the same name that
     * UE or other plugins may have loaded first (notably
     * Engine/Binaries/Win64/DML/x64/DirectML.dll — different version
     * than ours but same base name).
     */
    FString GetActualLoadedModulePath(const TCHAR* BaseName)
    {
        HMODULE Handle = GetModuleHandleW(BaseName);
        if (Handle == nullptr)
        {
            return FString(TEXT("(not loaded)"));
        }
        WCHAR PathBuf[MAX_PATH + 1] = {};
        const DWORD Len = GetModuleFileNameW(Handle, PathBuf, MAX_PATH);
        if (Len == 0 || Len >= MAX_PATH)
        {
            return FString(TEXT("(GetModuleFileName failed)"));
        }
        return FString(PathBuf);
    }

    /**
     * Verify that a loaded DLL came from the path we expected. Logs a
     * Warning if Windows' base-name cache served a different copy
     * (common signal: another plugin loaded its own DirectML.dll
     * before us, pinning that version into the process).
     */
    void VerifyLoadedPath(const TCHAR* BaseName, const FString& ExpectedFullPath)
    {
        const FString ActualPath = GetActualLoadedModulePath(BaseName);

        // Windows paths are case-insensitive and may use mixed separators.
        // Normalise both sides to forward-slash lower-case for comparison.
        auto Normalize = [](const FString& In) -> FString
        {
            FString Out = In;
            Out.ReplaceInline(TEXT("\\"), TEXT("/"));
            return Out.ToLower();
        };

        if (Normalize(ActualPath) == Normalize(ExpectedFullPath))
        {
            UE_LOG(LogInoAgents, Log,
                   TEXT("Onnx: Module: verified %s is loaded from %s"),
                   BaseName, *ActualPath);
        }
        else
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("Onnx: Module: BASE-NAME CACHE COLLISION — %s loaded from %s, ")
                   TEXT("but we wanted %s. Our preload didn't win the race (another plugin ")
                   TEXT("loaded a different %s first). Symptoms may include version-skew ")
                   TEXT("bugs at runtime."),
                   BaseName, *ActualPath, *ExpectedFullPath, BaseName);
        }
    }

    /**
     * Windows-only: our Binaries/ThirdParty/InoOnnxRuntime/Win64 directory.
     * Used to build full paths for the DirectML.dll + shared-providers
     * preloads below. Returns empty if the plugin can't be resolved.
     */
    FString ResolveWin64BinDir()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return FPaths::Combine(
            Plugin->GetBaseDir(),
            TEXT("Binaries/ThirdParty/InoOnnxRuntime/Win64"));
    }

    /**
     * Pre-load a sibling DLL by full path so Windows' loaded-modules
     * cache is populated with OUR copy for that base name.
     *
     * Historical context: DirectML.dll is a DELAY-LOAD dependency of
     * our ORT (confirmed via pefile parse — DIRECTORY_ENTRY_DELAY_IMPORT,
     * not static). Delay-loads resolve on the first call into the DLL,
     * not at ORT-load time, but they still go through Windows'
     * base-name DLL cache. UE 5.7's NNE plugin / RuntimeMetaHumanLipSync
     * / etc. LoadLibrary their own DirectML.dll early in editor startup,
     * winning that cache race — so ORT's first delay-load call of a
     * DirectML function would end up binding to THEIR version, causing
     * the MultiHeadAttention / Slice E_INVALIDARG and fp16 silent-noise
     * symptoms we hit in testing.
     *
     * The current architecture RENAMES DirectML.dll to InoDml.dll and
     * patches our ORT DLL's delay-import table (patch-ort-dml-import.py)
     * to match — so no other plugin looks for "InoDml.dll" and no cache
     * collision is possible. This preload is now belt-and-braces: it
     * ensures our InoDml.dll is in the cache under a known full path
     * before ORT's delay-load stub fires.
     *
     * Also pre-loads onnxruntime_providers_shared.dll for the same
     * defensive reason — ORT LoadLibrary's it lazily for certain
     * shared EPs.
     *
     * Failures are logged but non-fatal — if a preload DLL genuinely
     * isn't staged (shouldn't happen post-setup-script) we log clearly
     * and let the subsequent InoOnnxRuntime.dll load fail naturally
     * with a load error. Preloading a missing optional file just
     * downgrades DML availability; the CPU fallback still works.
     */
    void PreloadWin64Deps()
    {
        const FString BinDir = ResolveWin64BinDir();
        if (BinDir.IsEmpty())
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("Onnx: Module: cannot resolve plugin bin dir; InoDml.dll preload skipped"));
            return;
        }

        UE_LOG(LogInoAgents, Verbose,
               TEXT("Onnx: Module: preloading Win64 sibling DLLs from %s"),
               *BinDir);

        // Preload order doesn't matter for the two sibling DLLs — neither
        // imports the other. What matters is that both are loaded by full
        // path BEFORE InoOnnxRuntime.dll, so their base-name cache entries
        // are ours. "InoDml.dll" is OUR rename of DirectML.dll — see the
        // patch-ort-dml-import.py script for the import-table rewrite
        // that ties this together.
        struct FPreload { const TCHAR* Name; bool bRequired; };
        const FPreload Preloads[] = {
            { TEXT("InoDml.dll"),                      true  },
            { TEXT("onnxruntime_providers_shared.dll"), false },
        };

        for (const FPreload& P : Preloads)
        {
            const FString FullPath = FPaths::Combine(BinDir, P.Name);
            UE_LOG(LogInoAgents, Verbose,
                   TEXT("Onnx: Module: attempting preload of %s (full path=%s, required=%s)"),
                   P.Name, *FullPath, P.bRequired ? TEXT("yes") : TEXT("no"));
            void* Handle = FPlatformProcess::GetDllHandle(*FullPath);
            if (Handle != nullptr)
            {
                UE_LOG(LogInoAgents, Log,
                       TEXT("Onnx: Module: pre-loaded %s"), P.Name);

                // Now verify that the base-name cache actually served
                // OUR copy and not some earlier-loaded conflicting DLL.
                // If UE's NNE plugin / Marketplace plugin / anything
                // else LoadLibrary'd a DLL with the same base name
                // BEFORE us, Windows' cache returns THAT handle even
                // though we passed a full path — and our subsequent
                // InoOnnxRuntime.dll load would bind to the wrong
                // version's static imports.
                VerifyLoadedPath(P.Name, FullPath);

                // We deliberately DON'T FreeDllHandle — we want the module
                // to stay resident until process exit, holding the cache
                // entry for the whole lifetime of the game. Letting the
                // Windows loader clean up at process shutdown is fine.
            }
            else if (P.bRequired)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("Onnx: Module: REQUIRED preload failed: %s (path=%s). ")
                       TEXT("DirectML support will not work — our ORT's static import ")
                       TEXT("of %s will fail at InoOnnxRuntime.dll load time. Run ")
                       TEXT("Plugins/InoAgents/OnnxRuntime/scripts/setup-onnxruntime.ps1 ")
                       TEXT("to stage the binary."),
                       P.Name, *FullPath, P.Name);
            }
            else
            {
                UE_LOG(LogInoAgents, Verbose,
                       TEXT("Onnx: Module: optional preload not found: %s (path=%s)"),
                       P.Name, *FullPath);
            }
        }
    }
#endif // PLATFORM_WINDOWS

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
                   TEXT("Onnx: Module: OrtApi::GetAvailableProviders failed: %s"),
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
               TEXT("Onnx: Module: available providers (%d): %s"),
               NumProviders,
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
                   TEXT("Onnx: Module: OrtApi::ReleaseAvailableProviders returned an error (ignored): %s"),
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
                   TEXT("Onnx: Module: OrtGetApiBase returned nullptr. The loaded ONNX Runtime is broken."));
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
                   TEXT("Onnx: Module: OrtApiBase::GetApi(ORT_API_VERSION=%u) returned nullptr — ")
                   TEXT("the loaded ONNX Runtime does not implement this API version. ")
                   TEXT("Expected our pinned build (see Plugins/InoAgents/OnnxRuntime/ONNXRUNTIME_VERSION)."),
                   (uint32)ORT_API_VERSION);
            return nullptr;
        }

        UE_LOG(LogInoAgents, Verbose,
               TEXT("Onnx: Module: OrtApiBase::GetApi(ORT_API_VERSION=%u) resolved OrtApi vtable"),
               (uint32)ORT_API_VERSION);
        return Api;
    }
}

void* Init()
{
    UE_LOG(LogInoAgents, Log,
           TEXT("Onnx: Module: Init — loading ONNX Runtime DLLs (compiled-against ORT_API_VERSION=%u)"),
           (uint32)ORT_API_VERSION);

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

#if PLATFORM_WINDOWS
    // Pre-load DirectML.dll (+ onnxruntime_providers_shared.dll) by full
    // path BEFORE loading InoOnnxRuntime.dll. DirectML.dll is a static
    // import of the DML-flavored ORT — Windows resolves it at LoadLibrary
    // time from the cached loaded-modules table keyed by base name. The
    // preload seeds that cache with OUR copy, guaranteeing the main ORT
    // load binds to the DirectML version we ship rather than any other
    // plugin's or UE engine-dir copy. See PreloadWin64Deps for the full
    // rationale.
    PreloadWin64Deps();
#endif

    const FString LibName = ResolveOnnxLibraryName();
    if (LibName.IsEmpty())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("Onnx: Module: could not resolve ONNX Runtime library name (IPluginManager failed?)."));
        return nullptr;
    }

    UE_LOG(LogInoAgents, Verbose,
           TEXT("Onnx: Module: resolved library path: %s"),
           *LibName);

    void* Handle = FPlatformProcess::GetDllHandle(*LibName);
    if (Handle == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("Onnx: Module: failed to load %s. ")
               TEXT("Did you run Plugins/InoAgents/OnnxRuntime/scripts/setup-onnxruntime.ps1 ")
               TEXT("and re-package?"),
               *LibName);
        return nullptr;
    }

    UE_LOG(LogInoAgents, Log,
           TEXT("Onnx: Module: loaded %s"),
           *LibName);

#if PLATFORM_WINDOWS
    // Verify our ORT DLL won the base-name cache against UE's NNE
    // plugin copy (which ships a different ORT version). Same reason
    // we run the verification on DirectML.dll in PreloadWin64Deps.
    VerifyLoadedPath(TEXT("InoOnnxRuntime.dll"), LibName);
#endif

    // Resolve the single entry-point symbol we need. Everything else
    // goes through the OrtApi vtable returned by GetApiBase()->GetApi().
    // GetDllExport is UE's cross-platform wrapper over
    // GetProcAddress (Windows) / dlsym (Android).
    using OrtGetApiBaseFn = const OrtApiBase* (*)();
    void* EntryPoint = FPlatformProcess::GetDllExport(Handle, TEXT("OrtGetApiBase"));
    if (EntryPoint == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("Onnx: Module: %s does not export OrtGetApiBase. ")
               TEXT("The library is malformed or the setup script picked up the wrong file."),
               *LibName);
        FPlatformProcess::FreeDllHandle(Handle);
        return nullptr;
    }

    UE_LOG(LogInoAgents, Verbose,
           TEXT("Onnx: Module: GetDllExport(\"OrtGetApiBase\") resolved at %p"),
           EntryPoint);

    const OrtApiBase* ApiBase = reinterpret_cast<OrtGetApiBaseFn>(EntryPoint)();
    if (ApiBase != nullptr && ApiBase->GetVersionString != nullptr)
    {
        // ApiBase::GetVersionString reports the runtime version of the
        // loaded ORT DLL (what the binary is). We compare to
        // ORT_API_VERSION which is what we COMPILED against. A
        // mismatch here is a smoking gun for "Windows served a cached
        // copy of a different ORT version instead of ours" or "the
        // setup script staged a wrong-version DLL."
        const char* RuntimeVer = ApiBase->GetVersionString();
        UE_LOG(LogInoAgents, Log,
               TEXT("Onnx: Module: ORT runtime version: %s (compiled-against ORT_API_VERSION=%u)"),
               UTF8_TO_TCHAR(RuntimeVer != nullptr ? RuntimeVer : "<null>"),
               (uint32)ORT_API_VERSION);
    }

    GOrtApi = SelectOrtApi(ApiBase);
    if (GOrtApi == nullptr)
    {
        FPlatformProcess::FreeDllHandle(Handle);
        return nullptr;
    }

    LogAvailableProviders(GOrtApi);

    UE_LOG(LogInoAgents, Log,
           TEXT("Onnx: Module: Init complete"));
    return Handle;

#else
    // iOS / Linux / macOS: InoOnnxRuntime.Build.cs has no platform branch
    // yet, so the .so/.dylib isn't staged. Any GetApi() caller will see
    // nullptr and handle it gracefully.
    UE_LOG(LogInoAgents, Warning,
           TEXT("Onnx: Module: ONNX Runtime is not yet available on this platform."));
    return nullptr;
#endif
}

void Shutdown(void* Handle)
{
    UE_LOG(LogInoAgents, Log,
           TEXT("Onnx: Module: Shutdown — releasing OrtEnv and unloading DLL"));

    // Release the global OrtEnv (if any) BEFORE clearing GOrtApi — the
    // release calls Api->ReleaseEnv, which needs a valid API pointer.
    // ReleaseGlobalOrtEnv is a no-op if the env was never lazy-created.
    Internal::ReleaseGlobalOrtEnv();

    // Clear the cached OrtApi pointer next so any late callers of
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
        UE_LOG(LogInoAgents, Verbose,
               TEXT("Onnx: Module: FreeDllHandle released ONNX Runtime handle"));
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
