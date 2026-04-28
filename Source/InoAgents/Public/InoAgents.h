// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Modules/ModuleManager.h"

/**
 * InoAgents runtime module.
 *
 * Initializes ONNX Runtime and llama.cpp at StartupModule so the rest of the
 * plugin can call into them. LiteRT and LiteRT-LM are NOT loaded here —
 * the separate `InoLiteRT` plugin owns those DLLs/.so files and pre-loads
 * them at LoadingPhase=PreLoadingScreen, which runs before this module's
 * Default-phase StartupModule. By the time we get here, the LiteRT-LM
 * runtime is already callable.
 *
 * Two runtimes managed by this module:
 *   - ONNX Runtime (`InoOnnxRuntime` external module, dynamic-loaded via
 *     InoAgents::Onnx::Init() — see Source/InoAgents/Private/Onnx/InoOnnxModule.h).
 *     First consumer: Chatterbox Turbo TTS.
 *   - llama.cpp (`InoLlamaCpp` external module, dynamic-loaded via
 *     InoAgents::LlamaCpp::Init() — see Source/InoAgents/Private/LlamaCpp/InoLlamaCppModule.h).
 *     First consumer: NeuTTS Nano TTS.
 *
 * This module does NOT block on model loading — that happens lazily in
 * UInoLiteRtLmSubsystem::LoadModelAsync / UInoChatterboxTtsSubsystem /
 * UInoNeuTtsNanoSubsystem, off the game thread. Loading a 3 GB Gemma 4
 * model from here would freeze the editor for seconds.
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
    /** Handle to onnxruntime.dll, returned by InoAgents::Onnx::Init().
     *  nullptr on Android (the OS linker owns the .so) or if the load failed. */
    void* OnnxRuntimeHandle          = nullptr;
};
