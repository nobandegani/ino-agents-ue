// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Modules/ModuleManager.h"

/**
 * InoNeuTTS — Neuphonic NeuTTS Nano on-device TTS (LiteRT-LM backbone
 * + LiteRT NeuCodec decoder).
 *
 * One of the backend modules under the InoAgents plugin, structured so
 * each backend is a self-contained deprecation unit: deleting this
 * module's directory + its entry in InoAgents.uplugin's Modules array
 * removes the entire NeuTTS implementation cleanly.
 *
 * Loading phase: Default (after InoAgents core which holds the shared
 * LogInoAgents category). The LiteRT + LiteRT-LM C API DLLs are
 * pre-loaded at PreLoadingScreen by the sibling InoLiteRT plugin, so
 * by the time this module's StartupModule runs both C APIs are
 * callable.
 *
 * Logs to the shared LogInoAgents category (declared in InoAgents'
 * `InoAgentsLog.h`) — same as InoLiteRtLm. There is no
 * `LogInoNeuTTS`; filter by "[NeuTTS]" prefix in messages if you need
 * sub-module-level grep.
 *
 * Public surface lives under Source/InoNeuTTS/Public/NeuTTS/:
 *   - InoNeuTTSTypes.h           types + delegates
 *   - InoNeuTTSSubsystem.h       UInoNeuTTSSubsystem (model owner,
 *                                active-voice state, synth driver)
 *   - InoNeuTTSSynthesize.h      one-shot BP async action
 *   - InoNeuTTSStreamSynthesize.h streaming BP async action
 *
 * Settings live in Public/InoNeuTTSSettings.h (a separate
 * UDeveloperSettings page from the InoAgents one), voice asset in
 * Public/InoNeuTTSVoiceAsset.h.
 *
 * See Plugins/InoAgents/CLAUDE.md for architecture + build notes.
 */
class FInoNeuTTSModule : public IModuleInterface
{
public:
    //~ IModuleInterface
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    //~ End of IModuleInterface
};
