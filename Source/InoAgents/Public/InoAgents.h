// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Modules/ModuleManager.h"

/**
 * InoAgents runtime module.
 *
 * All three foundation runtimes the plugin consumes are owned by sibling
 * plugins and pre-loaded at LoadingPhase=PreLoadingScreen, before this
 * Default-phase StartupModule:
 *   - LiteRT + LiteRT-LM — provided by the `InoLiteRT` plugin.
 *   - ONNX Runtime — provided by the `InoOnnx` plugin (consumed via
 *     InoOnnx::GetApi() — see "InoOnnx.h").
 *   - llama.cpp — provided by the `InoLlama` plugin (consumed via
 *     InoAgents::LlamaCpp::GetApi() — see "InoLlama.h").
 *
 * This module's StartupModule is currently a no-op — every runtime's
 * lifecycle is owned upstream. Subsystem-level work (model loading,
 * conversation lifecycle) happens lazily off the game thread in
 * UInoLiteRtLmSubsystem::LoadModelAsync, UInoChatterboxTtsSubsystem,
 * UInoNeuTtsNanoNativeSubsystem, etc.
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
};
