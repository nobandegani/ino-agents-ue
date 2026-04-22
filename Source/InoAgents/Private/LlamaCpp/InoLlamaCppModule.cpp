// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoLlamaCppModule.h"

#include "InoAgentsLog.h"

#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

#include "Containers/StringConv.h"

#if PLATFORM_WINDOWS
    // For GetModuleHandleW / GetModuleFileNameW — used to verify which
    // DLL Windows' base-name cache actually served when we asked to
    // load a full-path DLL. Mirrors the verification pattern in
    // InoOnnxModule.cpp.
    #include "Windows/AllowWindowsPlatformTypes.h"
    #include <windows.h>
    #include "Windows/HideWindowsPlatformTypes.h"
#endif

// llama.cpp C API headers — needed for struct forward-decls
// (ggml_backend_reg) and nothing else. We don't call any llama_* or
// ggml_* function from this TU directly; everything goes through the
// function-pointer vtable resolved at runtime.
#include "ggml-backend.h"

namespace InoAgents::LlamaCpp
{

namespace
{
    /** Cached function-pointer vtable. Populated by Init(), cleared by Shutdown(). */
    FLlamaCppApi GApi;

    /** True iff Init() completed successfully. Read by GetApi(). */
    bool GApiValid = false;

    /**
     * DLL handles we own for the lifetime of the module. Opened by Init()
     * via FPlatformProcess::GetDllHandle (which reference-counts), freed
     * by Shutdown(). The order of members matches the recommended
     * Windows load order.
     *
     * On Android, only LlamaMain is populated; the Android linker
     * manages libggml*.so load/unload transparently.
     */
    struct FDllHandles
    {
        void* LibOmp      = nullptr;   // libomp140.x86_64.dll  (Windows only)
        void* GgmlBase    = nullptr;   // ggml-base.dll / libggml-base.so
        void* Ggml        = nullptr;   // ggml.dll / libggml.so
        void* GgmlVulkan  = nullptr;   // ggml-vulkan.dll (Windows only)
        void* LlamaMain   = nullptr;   // llama.dll / libllama.so
    };
    FDllHandles GHandles;

    /**
     * Per-platform library name we feed to FPlatformProcess::GetDllHandle
     * for the main llama library.
     *
     * Windows: absolute path to our staged llama.dll, resolved via
     *   IPluginManager so the loader can't pick up some other copy that
     *   happens to be on PATH.
     *
     * Android: bare soname. Android's dynamic linker resolves this via
     *   the APK's lib/<abi>/ dir (part of LD_LIBRARY_PATH for the
     *   process). The UPL's soLoadLibrary preload has already mapped
     *   the .so into the process, so dlopen just returns the existing
     *   handle.
     */
    FString ResolveMainLibraryPath()
    {
#if PLATFORM_WINDOWS
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return FPaths::Combine(
            Plugin->GetBaseDir(),
            TEXT("Binaries/ThirdParty/InoLlamaCpp/Win64"),
            TEXT("llama.dll"));
#elif PLATFORM_ANDROID
        return FString(TEXT("libllama.so"));
#else
        return FString();
#endif
    }

#if PLATFORM_WINDOWS
    /**
     * Windows-only: our Binaries/ThirdParty/InoLlamaCpp/Win64 directory.
     * Used both for preloading sibling DLLs and for
     * ggml_backend_load_all_from_path at init.
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
            TEXT("Binaries/ThirdParty/InoLlamaCpp/Win64"));
    }

    /**
     * Windows-only: query Windows for the full on-disk path of an
     * already-loaded DLL (by its base name). Returns empty if the DLL
     * isn't loaded, or a sentinel string on API failure. Mirrors
     * InoOnnxModule::GetActualLoadedModulePath.
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
     * Warning if Windows' base-name cache served a different copy —
     * signals a future collision with another plugin's same-named DLL.
     * Mirrors InoOnnxModule::VerifyLoadedPath.
     */
    void VerifyLoadedPath(const TCHAR* BaseName, const FString& ExpectedFullPath)
    {
        const FString ActualPath = GetActualLoadedModulePath(BaseName);

        auto Normalize = [](const FString& In) -> FString
        {
            FString Out = In;
            Out.ReplaceInline(TEXT("\\"), TEXT("/"));
            return Out.ToLower();
        };

        if (Normalize(ActualPath) == Normalize(ExpectedFullPath))
        {
            UE_LOG(LogInoAgents, Log,
                   TEXT("InoAgents: verified %s is loaded from %s"),
                   BaseName, *ActualPath);
        }
        else
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("InoAgents: BASE-NAME CACHE COLLISION — %s loaded from %s, ")
                   TEXT("but we wanted %s. Our preload didn't win the race (another plugin ")
                   TEXT("loaded a different %s first). Symptoms may include version-skew ")
                   TEXT("bugs at runtime."),
                   BaseName, *ActualPath, *ExpectedFullPath, BaseName);
        }
    }

    /**
     * Pre-load every sibling DLL that llama.dll transitively depends on
     * or that llama.cpp's internal backend loader will LoadLibraryA by
     * bare filename at runtime. All by full path so Windows' loaded-
     * modules cache is populated with OUR copies keyed by base name
     * BEFORE anything else in the process can race.
     *
     * Load order must match the dependency graph (top of list = deepest
     * dependency, loaded first):
     *
     *   libomp140.x86_64.dll  — MSVC OpenMP redist. Imported by the
     *                           ggml-cpu-*.dll variants that use OpenMP
     *                           for CPU threading. Must be loaded before
     *                           any ggml-cpu DLL is dlopen'd by the
     *                           backend loader.
     *   ggml-base.dll         — Core ggml (tensor ops, memory, etc.).
     *                           No deps on our other libs.
     *   ggml.dll              — Dispatcher; depends on ggml-base.dll.
     *                           Imports the backend API + exports the
     *                           ggml_backend_* entry points we resolve.
     *   ggml-vulkan.dll       — Vulkan backend; depends on ggml +
     *                           ggml-base + OS-provided vulkan-1.dll.
     *                           Preloaded so we don't depend on the
     *                           backend loader's directory glob to
     *                           find it (simpler + more predictable
     *                           than letting ggml_backend_load_all
     *                           discover it).
     *   llama.dll             — Main library; depends on ggml.dll.
     *                           Exports the llama_* entry points.
     *
     * The 14 ggml-cpu-*.dll CPU variants are intentionally NOT preloaded
     * here. ggml_backend_load_all_from_path (called at the end of
     * Init()) glob-scans the directory for ggml-*.dll and dlopens each
     * one individually — each variant's init probes the host CPU and
     * rejects itself if the required instruction set isn't available.
     * Only the variants that pass survive in the registered-backends
     * list; the rest are silently unregistered.
     *
     * Failures on non-required preloads are logged at Warning and do
     * not abort Init. libomp is required (CPU variants won't load
     * without it); the others fail as Errors.
     */
    bool PreloadWin64Deps(const FString& BinDir, FDllHandles& OutHandles)
    {
        struct FPreload
        {
            const TCHAR* Name;
            bool         bRequired;
            void**       StoreHandleAt;
        };

        const FPreload Preloads[] = {
            { TEXT("libomp140.x86_64.dll"), true,  &OutHandles.LibOmp     },
            { TEXT("ggml-base.dll"),        true,  &OutHandles.GgmlBase   },
            { TEXT("ggml.dll"),             true,  &OutHandles.Ggml       },
            { TEXT("ggml-vulkan.dll"),      false, &OutHandles.GgmlVulkan },
        };

        for (const FPreload& P : Preloads)
        {
            const FString FullPath = FPaths::Combine(BinDir, P.Name);
            void* Handle = FPlatformProcess::GetDllHandle(*FullPath);
            if (Handle != nullptr)
            {
                *(P.StoreHandleAt) = Handle;
                UE_LOG(LogInoAgents, Log,
                       TEXT("InoAgents: pre-loaded %s"), P.Name);
                VerifyLoadedPath(P.Name, FullPath);
            }
            else if (P.bRequired)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("InoAgents: REQUIRED preload failed: %s (path=%s). ")
                       TEXT("llama.cpp will not initialise. Run ")
                       TEXT("Plugins/InoAgents/LlamaCpp/scripts/setup-llamacpp.ps1 ")
                       TEXT("to stage the binaries."),
                       P.Name, *FullPath);
                return false;
            }
            else
            {
                UE_LOG(LogInoAgents, Warning,
                       TEXT("InoAgents: optional preload not found: %s (path=%s). ")
                       TEXT("The corresponding backend (Vulkan) will be unavailable; CPU inference still works."),
                       P.Name, *FullPath);
            }
        }
        return true;
    }
#endif // PLATFORM_WINDOWS

    /**
     * Resolve a function pointer by searching a list of DLL handles in
     * order. Returns nullptr if none of the handles export the symbol.
     *
     * Order matters: for symbols that may be exported from multiple DLLs
     * (theoretically possible if future llama.cpp re-exports) we want
     * the "closest" / most-specific handle first. Our list (Llama, Ggml,
     * GgmlBase) matches the typical layer ordering.
     */
    void* ResolveExport(const TCHAR* Name, std::initializer_list<void*> Handles)
    {
        for (void* H : Handles)
        {
            if (H == nullptr) continue;
            if (void* P = FPlatformProcess::GetDllExport(H, Name))
            {
                return P;
            }
        }
        return nullptr;
    }

    /**
     * Populate every function pointer in OutApi by resolving against the
     * given DLL handles. Returns true if every required pointer was
     * resolved. Errors on any missing required pointer include enough
     * context to diagnose "wrong llama.cpp version" vs "we asked for a
     * symbol that the staged build doesn't ship."
     */
    bool ResolveApi(FLlamaCppApi& OutApi, const FDllHandles& Handles)
    {
        // llama_* symbols live in llama.dll; ggml_* symbols live in
        // ggml.dll / ggml-base.dll. The search list covers both so the
        // RESOLVE macro below doesn't need to know which is which.
        const std::initializer_list<void*> SearchList = {
            Handles.LlamaMain,
            Handles.Ggml,
            Handles.GgmlBase,
        };

        bool bAllOk = true;

        auto ResolveOne = [&](const TCHAR* Name, void** Slot) -> bool
        {
            void* P = ResolveExport(Name, SearchList);
            *Slot = P;
            if (P == nullptr)
            {
                UE_LOG(LogInoAgents, Error,
                       TEXT("InoAgents: llama.cpp is missing required export: %s. ")
                       TEXT("Staged build at LlamaCpp/LLAMACPP_VERSION may be the wrong version ")
                       TEXT("or was built with this symbol stripped."),
                       Name);
                bAllOk = false;
            }
            return P != nullptr;
        };

        // Macro: resolve + cast + null-check + error-log in one line.
        // Reads the function-pointer type from the struct member itself
        // via decltype, so there's no duplicated signature to drift.
        #define INO_RESOLVE_LLAMA(FuncName)                                   \
            do {                                                              \
                void* P_ = nullptr;                                           \
                ResolveOne(TEXT(#FuncName), &P_);                             \
                OutApi.FuncName = reinterpret_cast<decltype(OutApi.FuncName)>(P_); \
            } while (0)

        // Milestone C — module startup + backend discovery
        INO_RESOLVE_LLAMA(llama_backend_init);
        INO_RESOLVE_LLAMA(llama_backend_free);
        INO_RESOLVE_LLAMA(llama_print_system_info);
        INO_RESOLVE_LLAMA(ggml_backend_load_all_from_path);
        INO_RESOLVE_LLAMA(ggml_backend_reg_count);
        INO_RESOLVE_LLAMA(ggml_backend_reg_get);
        INO_RESOLVE_LLAMA(ggml_backend_reg_name);

        // NeuTTS prerequisite — model
        INO_RESOLVE_LLAMA(llama_model_default_params);
        INO_RESOLVE_LLAMA(llama_model_load_from_file);
        INO_RESOLVE_LLAMA(llama_model_free);
        INO_RESOLVE_LLAMA(llama_model_get_vocab);
        INO_RESOLVE_LLAMA(llama_model_desc);
        INO_RESOLVE_LLAMA(llama_model_n_ctx_train);

        // NeuTTS prerequisite — context
        INO_RESOLVE_LLAMA(llama_context_default_params);
        INO_RESOLVE_LLAMA(llama_init_from_model);
        INO_RESOLVE_LLAMA(llama_free);
        INO_RESOLVE_LLAMA(llama_n_ctx);

        // NeuTTS prerequisite — memory (KV-cache)
        INO_RESOLVE_LLAMA(llama_get_memory);
        INO_RESOLVE_LLAMA(llama_memory_clear);

        // NeuTTS prerequisite — vocab
        INO_RESOLVE_LLAMA(llama_vocab_n_tokens);
        INO_RESOLVE_LLAMA(llama_vocab_eos);
        INO_RESOLVE_LLAMA(llama_vocab_is_eog);
        INO_RESOLVE_LLAMA(llama_vocab_get_add_bos);
        INO_RESOLVE_LLAMA(llama_token_to_piece);

        // NeuTTS prerequisite — tokenize / detokenize
        INO_RESOLVE_LLAMA(llama_tokenize);
        INO_RESOLVE_LLAMA(llama_detokenize);

        // NeuTTS prerequisite — batch + decode
        INO_RESOLVE_LLAMA(llama_batch_init);
        INO_RESOLVE_LLAMA(llama_batch_free);
        INO_RESOLVE_LLAMA(llama_batch_get_one);
        INO_RESOLVE_LLAMA(llama_decode);
        INO_RESOLVE_LLAMA(llama_get_logits_ith);

        // NeuTTS prerequisite — sampler chain
        INO_RESOLVE_LLAMA(llama_sampler_chain_default_params);
        INO_RESOLVE_LLAMA(llama_sampler_chain_init);
        INO_RESOLVE_LLAMA(llama_sampler_chain_add);
        INO_RESOLVE_LLAMA(llama_sampler_init_greedy);
        INO_RESOLVE_LLAMA(llama_sampler_init_dist);
        INO_RESOLVE_LLAMA(llama_sampler_init_top_k);
        INO_RESOLVE_LLAMA(llama_sampler_init_top_p);
        INO_RESOLVE_LLAMA(llama_sampler_init_min_p);
        INO_RESOLVE_LLAMA(llama_sampler_init_temp);
        INO_RESOLVE_LLAMA(llama_sampler_sample);
        INO_RESOLVE_LLAMA(llama_sampler_accept);
        INO_RESOLVE_LLAMA(llama_sampler_free);

        #undef INO_RESOLVE_LLAMA

        return bAllOk;
    }
} // namespace

bool Init()
{
#if PLATFORM_WINDOWS || PLATFORM_ANDROID

#if PLATFORM_WINDOWS
    // Preload every sibling DLL by full path. Seeds Windows' base-name
    // cache with our copies before llama.dll's PE imports get resolved.
    const FString BinDir = ResolveWin64BinDir();
    if (BinDir.IsEmpty())
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("InoAgents: cannot resolve plugin bin dir; llama.cpp init aborted."));
        return false;
    }
    if (!PreloadWin64Deps(BinDir, GHandles))
    {
        // PreloadWin64Deps has already logged the specific failure.
        // Shutdown will free any handles that did load.
        return false;
    }
#endif // PLATFORM_WINDOWS

    // Load the main library (llama.dll / libllama.so) by full path on
    // Windows, bare soname on Android. On Android the soLoadLibrary UPL
    // directive has already mapped it — this just returns the existing
    // handle.
    const FString MainLibPath = ResolveMainLibraryPath();
    if (MainLibPath.IsEmpty())
    {
        UE_LOG(LogInoAgents, Warning,
               TEXT("InoAgents: could not resolve llama.cpp main library path (IPluginManager failed?)."));
        return false;
    }

    GHandles.LlamaMain = FPlatformProcess::GetDllHandle(*MainLibPath);
    if (GHandles.LlamaMain == nullptr)
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("InoAgents: failed to load %s. ")
               TEXT("Did you run Plugins/InoAgents/LlamaCpp/scripts/setup-llamacpp.ps1 ")
               TEXT("and re-package?"),
               *MainLibPath);
        return false;
    }
    UE_LOG(LogInoAgents, Log, TEXT("InoAgents: loaded %s"), *MainLibPath);

#if PLATFORM_WINDOWS
    VerifyLoadedPath(TEXT("llama.dll"), MainLibPath);
#endif

    // Resolve every function-pointer in the vtable by searching the
    // loaded handles. Aborts if any required symbol is missing.
    if (!ResolveApi(GApi, GHandles))
    {
        UE_LOG(LogInoAgents, Error,
               TEXT("InoAgents: llama.cpp API resolution failed; aborting init."));
        return false;
    }

    // Register all backends. On Windows this scans our staged bin dir
    // for ggml-*.dll (14 CPU variants + ggml-vulkan). On Android the
    // .so files are co-located in the APK's lib/arm64-v8a/ dir — but
    // we can't give ggml_backend_load_all_from_path that path directly
    // (it's not reachable until load time and varies per-install), so
    // we fall back to ggml_backend_load_all's default search behaviour
    // via the one-arg version. For Milestone C, simpler: on Android
    // call the _from_path variant with an empty string so it picks
    // its internal default (normally the executable's directory),
    // which on Android doesn't help — but libggml.so's DT_NEEDED
    // already resolved the main backends, and the CPU variants will
    // be discovered via the system linker's default search in
    // subsequent milestones. For now the smoke test is sufficient
    // on Windows.
    if (GApi.ggml_backend_load_all_from_path != nullptr)
    {
#if PLATFORM_WINDOWS
        const FTCHARToUTF8 BinDirUtf8(*BinDir);
        GApi.ggml_backend_load_all_from_path(BinDirUtf8.Get());
        UE_LOG(LogInoAgents, Log,
               TEXT("InoAgents: ggml_backend_load_all_from_path(%s) invoked."),
               *BinDir);
#else
        // Android: pass nullptr so upstream falls back to its default
        // (which typically scans the exe's dir — harmless if it finds
        // nothing).
        GApi.ggml_backend_load_all_from_path(nullptr);
        UE_LOG(LogInoAgents, Log,
               TEXT("InoAgents: ggml_backend_load_all_from_path(nullptr) invoked (Android default search)."));
#endif
    }

    // Initialise llama.cpp's runtime globals. Must happen after backend
    // registration so the default backend is known. Upstream documents
    // this as idempotent + cheap.
    if (GApi.llama_backend_init != nullptr)
    {
        GApi.llama_backend_init();
    }

    // Summary log — one line so the startup output stays readable.
    if (GApi.llama_print_system_info != nullptr)
    {
        const char* Info = GApi.llama_print_system_info();
        UE_LOG(LogInoAgents, Log,
               TEXT("InoAgents: llama.cpp initialised — %s"),
               Info != nullptr ? UTF8_TO_TCHAR(Info) : TEXT("(null system info)"));
    }
    else
    {
        UE_LOG(LogInoAgents, Log, TEXT("InoAgents: llama.cpp initialised."));
    }

    GApiValid = true;
    return true;

#else
    // iOS / Linux / macOS: InoLlamaCpp.Build.cs has no platform branch
    // yet. Any GetApi() caller will see nullptr and handle gracefully.
    UE_LOG(LogInoAgents, Warning,
           TEXT("InoAgents: llama.cpp is not yet available on this platform."));
    return false;
#endif
}

void Shutdown()
{
    // Clear the "valid" flag first so any late callers of GetApi()
    // see nullptr rather than a vtable belonging to a DLL we are about
    // to unload. Happens-before ordering matters here.
    const bool bWasValid = GApiValid;
    GApiValid = false;

    // Tear down llama.cpp's runtime globals BEFORE unloading the DLL.
    // llama_backend_free is a no-op if llama_backend_init was never
    // called — upstream guards it.
    if (bWasValid && GApi.llama_backend_free != nullptr)
    {
        GApi.llama_backend_free();
    }

    // Zero the vtable now — any post-Shutdown access goes through
    // nullptrs, which at least crashes deterministically rather than
    // dereferencing into a freed code page.
    GApi = FLlamaCppApi{};

#if PLATFORM_WINDOWS || PLATFORM_ANDROID
    // Release in reverse dependency order: top of the graph first so
    // Windows doesn't error out on "can't free libomp, it's still
    // imported by ggml-cpu-* which is referenced by ggml.dll".
    auto Free = [](void*& Handle)
    {
        if (Handle != nullptr)
        {
            FPlatformProcess::FreeDllHandle(Handle);
            Handle = nullptr;
        }
    };
    Free(GHandles.LlamaMain);
    Free(GHandles.GgmlVulkan);
    Free(GHandles.Ggml);
    Free(GHandles.GgmlBase);
    Free(GHandles.LibOmp);
#endif
}

const FLlamaCppApi* GetApi()
{
    return GApiValid ? &GApi : nullptr;
}

} // namespace InoAgents::LlamaCpp
