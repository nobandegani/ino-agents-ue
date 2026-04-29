// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Modules/ModuleManager.h"

/**
 * InoChatterboxNative — Chatterbox Turbo TTS via ONNX Runtime.
 *
 * One of (eventually) several backend modules under the InoAgents
 * plugin, structured so each backend is a self-contained deprecation
 * unit: deleting this module's directory + its entry in
 * InoAgents.uplugin's Modules array removes the entire ONNX-flavored
 * Chatterbox implementation cleanly.
 *
 * Loading phase: Default (after InoAgents core which holds the shared
 * LogInoAgents category). The ONNX Runtime DLLs are pre-loaded at
 * PreLoadingScreen by the sibling InoOnnx plugin, so by the time this
 * module's StartupModule runs, OrtApi is callable.
 *
 * Public surface lives under Source/InoChatterboxNative/Public/Chatterbox/:
 *   - InoChatterboxTurboNativeTypes.h            variant enum, voice/options/result USTRUCTs
 *   - InoChatterboxTurboNativeSubsystem.h     UInoChatterboxTurboNativeSubsystem (UGameInstanceSubsystem)
 *   - InoChatterboxTurboNativeStreamSynthesize.h async-action wrapper
 *
 * Settings live in Public/InoChatterboxNativeSettings.h (a separate
 * UDeveloperSettings page from the InoAgents one).
 */
class FInoChatterboxNativeModule : public IModuleInterface
{
public:
    //~ IModuleInterface
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    //~ End of IModuleInterface
};
