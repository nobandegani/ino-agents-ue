// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

// Forward-declare the ggml backend-registration handle so this header
// doesn't pull in ggml-backend.h. The definition comes from that header
// in the .cpp consumers.
struct ggml_backend_reg;

/**
 * llama.cpp module startup / shutdown glue + global API vtable accessor.
 *
 * Called from FInoAgentsModule::StartupModule and ::ShutdownModule to
 * bring the llama.cpp runtime up alongside LiteRT-LM and ONNX Runtime.
 *
 * Unlike ORT — which exposes a single vendor-supplied vtable (OrtApi)
 * reached via one exported entry point (OrtGetApiBase) — llama.cpp
 * exports every function individually with no central dispatcher. We
 * synthesise our own vtable (FLlamaCppApi) by resolving each needed
 * function pointer at module startup. Consumers call Api-> methods,
 * gated on a null check against GetApi().
 *
 * The vtable grows across the milestone sequence:
 *   Milestone C (this file):   init/free/system-info/backend-enum + the
 *                              backend auto-loader.
 *   Milestone D:               tokenize/decode/sample/chat-template —
 *                              the raw C API surface used by the
 *                              subsystem's worker thread.
 *
 * Init() on Windows:
 *   - PreloadWin64Deps(): loads libomp140.x86_64.dll, ggml-base.dll,
 *     ggml.dll, ggml-vulkan.dll by full path so Windows' loaded-modules
 *     cache is seeded with OUR copies before llama.dll's PE imports
 *     are resolved. Load order is intentional (dependencies first);
 *     Windows' base-name resolution for llama.dll's imports then finds
 *     ggml.dll by base name in the already-loaded cache rather than
 *     searching arbitrary DLL paths.
 *   - FPlatformProcess::GetDllHandle on llama.dll (full path).
 *   - FPlatformProcess::GetDllExport to resolve every function pointer
 *     in FLlamaCppApi. ggml_* symbols are exported from ggml.dll /
 *     ggml-base.dll, so we search across all the loaded llama.cpp
 *     handles for each name — first hit wins.
 *   - ggml_backend_load_all_from_path(bin_dir): scans our staging
 *     directory for ggml-*.dll and registers each as a backend. This
 *     is how the 14 CPU variants + ggml-vulkan get picked up — the
 *     runtime discards variants the host CPU doesn't support.
 *   - llama_backend_init(): initialises the llama.cpp runtime globals.
 *
 * Init() on Android:
 *   - No PreloadWin64Deps. UPL's <soLoadLibrary> has already preloaded
 *     libllama.so via System.loadLibrary, and libllama.so's DT_NEEDED
 *     chain cascaded libggml.so and libggml-base.so. The CPU variants
 *     (libggml-cpu-android_*.so) are staged in the APK's lib/arm64-v8a/
 *     but not preloaded — ggml_backend_load_all_from_path dlopens them
 *     on demand once we've resolved its function pointer.
 *   - GetDllHandle("libllama.so") returns the already-mapped handle.
 *   - Same GetDllExport + backend-loader + backend_init sequence.
 *
 * Init() on iOS / Linux / macOS:
 *   - Warns. GetApi() returns nullptr. Every consumer null-checks the
 *     return and handles gracefully.
 *
 * Returns true if everything resolved; false otherwise (individual
 * resolution failures are logged at Error level).
 *
 * Shutdown() is the inverse:
 *   - llama_backend_free()
 *   - FreeDllHandle on every handle we opened, in reverse load order.
 *   - Clears the cached API pointer.
 *   Idempotent — safe to call whether or not Init succeeded, and safe
 *   to call multiple times.
 *
 * GetApi() is the accessor every llama.cpp-consuming .cpp in the plugin
 * should use. Returns nullptr if llama.cpp failed to initialise;
 * consumers MUST null-check before calling into the vtable.
 */
namespace InoAgents::LlamaCpp
{
    /**
     * Function-pointer vtable for the subset of llama.cpp's C API that
     * the InoAgents plugin actually calls. Populated by Init() via
     * FPlatformProcess::GetDllExport against the loaded llama.cpp
     * handles; cleared to zeros by Shutdown().
     *
     * Grows over the milestone sequence — add members for Milestone D
     * (tokenize, decode, sampler chain, chat template, model/context
     * lifecycle) and wire their resolution in InoLlamaCppModule.cpp's
     * ResolveApi helper.
     *
     * Lifetime: the FLlamaCppApi instance lives in a file-static in
     * InoLlamaCppModule.cpp. The pointer returned by GetApi() is valid
     * from a successful Init() until Shutdown(). Multiple threads may
     * read freely (the pointer is set once at module startup and
     * cleared once at module shutdown; there are no writes in between).
     */
    struct FLlamaCppApi
    {
        // Runtime lifecycle. Called once each by Init() / Shutdown() on
        // the game thread during module bring-up/tear-down.
        void        (*llama_backend_init)(void) = nullptr;
        void        (*llama_backend_free)(void) = nullptr;

        // Build / system information. llama_print_system_info returns a
        // pointer to a static internal buffer owned by llama.cpp — valid
        // until the next call, so callers should copy if they want to
        // retain across calls.
        const char* (*llama_print_system_info)(void) = nullptr;

        // Backend loader: scans a directory for ggml-*.{dll,so} and
        // registers each as a backend. Exported from ggml-base.dll /
        // libggml-base.so. Called once by Init() after the module
        // handles are loaded to register the 14 Windows CPU variants +
        // ggml-vulkan (or the 7 Android ARM variants).
        void        (*ggml_backend_load_all_from_path)(const char* dir_path) = nullptr;

        // Backend enumeration — used by the BackendInfoTest smoke test
        // to log which backends actually registered successfully on the
        // host CPU / GPU.
        size_t                 (*ggml_backend_reg_count)(void) = nullptr;
        struct ggml_backend_reg* (*ggml_backend_reg_get)(size_t index) = nullptr;
        const char*            (*ggml_backend_reg_name)(struct ggml_backend_reg* reg) = nullptr;
    };

    /**
     * Load the llama.cpp runtime, resolve the API vtable, register
     * backends, and initialise llama's globals. Logs a summary line on
     * success; Errors on any failure.
     *
     * Returns true on full success, false if any required step failed.
     * Even on false the caller MUST still call Shutdown() to release
     * any partial state (open DLL handles, etc.) — Shutdown is
     * idempotent and handles the partial-init case.
     */
    bool Init();

    /**
     * Inverse of Init. Calls llama_backend_free (if the pointer was
     * resolved), releases every DLL handle we opened, and clears the
     * cached API pointer. Safe to call whether or not Init() ran or
     * succeeded.
     */
    void Shutdown();

    /**
     * Return the cached FLlamaCppApi vtable pointer, or nullptr if
     * Init() did not succeed (platform not supported, DLL load failed,
     * function resolution failed, etc.).
     *
     * Lifetime: valid from a successful Init() until Shutdown(). Threads
     * may read freely; the pointer itself is set once at module startup
     * and cleared once at module shutdown, with no writes in between.
     *
     * Callers MUST null-check before dereferencing.
     */
    const FLlamaCppApi* GetApi();
}
