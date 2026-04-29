// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Modules/ModuleManager.h"

/**
 * InoNeuTtsNative — Neuphonic NeuTTS Nano TTS via llama.cpp + NeuCodec.
 *
 * Sub-module of the InoAgents plugin, sibling to InoChatterboxOnnx.
 * Each backend implementation is a self-contained deprecation unit:
 * deleting this folder + dropping the entry in InoAgents.uplugin
 * removes the entire NeuTTS Nano impl cleanly.
 *
 * Loading phase: Default (after InoAgents core which holds the shared
 * LogInoAgents category). The llama.cpp + ONNX Runtime DLLs are
 * pre-loaded at PreLoadingScreen by the sibling InoLlama and InoOnnx
 * plugins, so by the time this module's StartupModule runs both
 * runtimes are callable.
 *
 * Public surface lives under Source/InoNeuTtsNative/Public/NeuTtsNano/:
 *   - InoNeuTtsNanoTypes.h        variant enum, voice/options/result USTRUCTs
 *   - InoNeuTtsNanoSubsystem.h    UInoNeuTtsNanoSubsystem (UGameInstanceSubsystem)
 *
 * Settings live in Public/InoNeuTtsNativeSettings.h (a separate
 * UDeveloperSettings page from the InoAgents one).
 */
class FInoNeuTtsNativeModule : public IModuleInterface
{
public:
    //~ IModuleInterface
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    //~ End of IModuleInterface
};
