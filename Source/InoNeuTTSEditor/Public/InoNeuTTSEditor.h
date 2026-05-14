// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Modules/ModuleManager.h"

/**
 * InoNeuTTSEditor — editor-only companion to the InoNeuTTS runtime
 * module.
 *
 * Hosts the UFactory that turns `.inv` source files (plain JSON, same
 * shape your Python encoder script writes) into UInoNeuTTSVoiceAsset
 * UAssets. UE auto-discovers the factory by its `Formats` array — no
 * AssetTools registration needed in StartupModule.
 *
 * Logs to the shared LogInoAgents category (declared in InoAgents'
 * `InoAgentsLog.h`) — same convention as the InoNeuTTS runtime module.
 *
 * Type=Editor in the .uplugin so this module is excluded from cooked
 * game / shipping builds.
 */
class FInoNeuTTSEditorModule : public IModuleInterface
{
public:
    //~ IModuleInterface
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    //~ End IModuleInterface
};
