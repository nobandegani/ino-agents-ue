// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Modules/ModuleManager.h"

/**
 * InoAgents runtime module.
 *
 * Loads the LiteRT-LM native runtime at StartupModule so the rest of the
 * plugin can call the C API (from <litert/lm/engine.h>) through the
 * delay-load trampolines set up in InoAgentsLibrary.Build.cs.
 *
 * Five DLLs are loaded on Windows:
 *   - libGemmaModelConstraintProvider.dll  (loaded first; a runtime dependency
 *                                           of LiteRtLm.dll, not a Bazel
 *                                           output but an upstream prebuilt)
 *   - LiteRtLm.dll                         (our monolithic Bazel output)
 *   - libLiteRt.dll                         (LiteRT core runtime; GPU DLLs
 *                                           import from this)
 *   - libLiteRtWebGpuAccelerator.dll        (WebGPU → D3D12 GPU accelerator)
 *   - libLiteRtTopKWebGpuSampler.dll        (GPU-side top-K sampling)
 *
 * The three GPU DLLs are pre-loaded with full absolute paths so that
 * when LiteRT's engine internally calls LoadLibraryA with just the
 * filename (e.g. "libLiteRtWebGpuAccelerator.dll"), Windows finds the
 * already-loaded module rather than searching the executable directory
 * (which is UE's Engine/Binaries/Win64/, not the plugin dir). Without
 * pre-loading, GPU backend=gpu fails silently because LoadLibraryA
 * can't find the DLLs.
 *
 * This module does NOT block on model loading — that happens lazily in
 * UInoLiteRtLmSubsystem::LoadModelAsync, off the game thread. Loading a 3 GB
 * Gemma 4 model from here would freeze the editor for seconds.
 *
 * See Plugins/InoAgents/README.md for the plugin's user-facing API and
 * Plugins/InoAgents/CLAUDE.md for the architecture + build notes.
 */
class FInoAgentsModule : public IModuleInterface
{
public:
    //~ IModuleInterface
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    //~ End of IModuleInterface

private:
    /** Handle to libGemmaModelConstraintProvider.dll. Nullptr if load failed. */
    void* GemmaConstraintProviderHandle = nullptr;

    /** Handle to LiteRtLm.dll. Nullptr if load failed. */
    void* LiteRtLmHandle = nullptr;

    // GPU accelerator DLLs — pre-loaded so the LiteRT engine's
    // internal LoadLibraryA can find them by filename. nullptr if the
    // prebuilt DLLs are not present (GPU will not be available, but
    // CPU still works fine).
    void* LiteRtHandle               = nullptr;
    void* WebGpuAcceleratorHandle    = nullptr;
    void* TopKWebGpuSamplerHandle    = nullptr;
};
