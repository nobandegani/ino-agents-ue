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
 * Two DLLs are loaded on Windows:
 *   - libGemmaModelConstraintProvider.dll  (loaded first; a runtime dependency
 *                                           of LiteRtLm.dll, not a Bazel
 *                                           output but an upstream prebuilt)
 *   - LiteRtLm.dll                         (our monolithic Bazel output)
 *
 * This module does NOT block on model loading — that happens lazily in
 * ULiteRtLmSubsystem::LoadModelAsync, off the game thread. Loading a 3 GB
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
};
