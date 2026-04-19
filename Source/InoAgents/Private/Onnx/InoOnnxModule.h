// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

// Forward-declare so this header doesn't pull in onnxruntime_c_api.h.
// The definition comes from that header in the .cpp consumers.
struct OrtApi;

/**
 * ONNX Runtime module startup / shutdown glue + global OrtApi accessor.
 *
 * Called from FInoAgentsModule::StartupModule and ::ShutdownModule to
 * bring the ORT runtime up alongside LiteRT-LM.
 *
 * Init() on Windows:
 *   - FPlatformProcess::GetDllHandle on the renamed
 *     Binaries/ThirdParty/InoOnnxRuntime/Win64/InoOnnxRuntime.dll.
 *     Renamed (from onnxruntime.dll) to dodge Windows' base-name DLL
 *     caching, which otherwise returns UE's NNE-bundled older ORT
 *     handle instead of ours.
 *   - FPlatformProcess::GetDllExport to resolve "OrtGetApiBase" — the
 *     only symbol we resolve dynamically. Everything else flows through
 *     the OrtApi vtable that GetApiBase()->GetApi(ORT_API_VERSION)
 *     hands back.
 *   - Caches the resulting OrtApi* for GetApi() callers.
 *
 * Init() on Android:
 *   - No DLL load. libonnxruntime.so is already resident via
 *     libUnreal.so's DT_NEEDED + the UPL <soLoadLibrary> preload.
 *   - Calls OrtGetApiBase() directly (linker-resolved via
 *     PublicAdditionalLibraries) and caches the OrtApi*.
 *
 * Init() on iOS / Linux / macOS:
 *   - Warns. GetApi() returns nullptr. Any consumer that calls GetApi()
 *     and checks the return handles this gracefully.
 *
 * Returns an opaque DLL handle on Windows (the caller should hand it
 * back to Shutdown for FreeDllHandle) or nullptr on any other outcome.
 *
 * Shutdown() is the inverse — FreeDllHandle on Windows, no-op elsewhere.
 * Idempotent: nullptr handles are fine. Shutdown also clears the cached
 * OrtApi* so callers see nullptr after tear-down.
 *
 * GetApi() is the accessor every other ORT-using .cpp in the plugin
 * should use. Returns nullptr if ORT failed to initialize; consumers
 * MUST null-check before using.
 */
namespace InoAgents::Onnx
{
    void* Init();
    void  Shutdown(void* Handle);

    /**
     * Return the cached OrtApi vtable pointer, or nullptr if Init() did
     * not succeed (platform not supported, DLL load failed, API version
     * mismatch, etc.). Callers must null-check.
     *
     * Lifetime: valid from a successful Init() until Shutdown(). Threads
     * may read freely; the pointer itself is set once at module startup
     * and cleared once at module shutdown, with no writes in between.
     */
    const OrtApi* GetApi();
}
