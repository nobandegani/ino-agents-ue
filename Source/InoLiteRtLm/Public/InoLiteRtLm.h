// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Modules/ModuleManager.h"

/**
 * InoLiteRtLm — Google LiteRT-LM Gemma 4 inference (chat, streaming,
 * tool calling).
 *
 * One of the backend modules under the InoAgents plugin, structured so
 * each backend is a self-contained deprecation unit: deleting this
 * module's directory + its entry in InoAgents.uplugin's Modules array
 * removes the entire LiteRT-LM implementation cleanly.
 *
 * Loading phase: Default (after InoAgents core which holds the shared
 * LogInoAgents category). The LiteRT-LM C API DLLs are pre-loaded at
 * PreLoadingScreen by the sibling InoLiteRT plugin, so by the time
 * this module's StartupModule runs the C API is callable.
 *
 * Public surface lives under Source/InoLiteRtLm/Public/LiteRtLm/:
 *   - InoLiteRtLmTypes.h            backend enum, model config / entry
 *                                    USTRUCTs, all delegates
 *   - InoLiteRtLmSubsystem.h        UInoLiteRtLmSubsystem (engine owner,
 *                                    tool registry, model loader)
 *   - InoLiteRtLmConversation.h     UInoLiteRtLmConversation (one
 *                                    stateful chat with history,
 *                                    streaming, tool-call agent loop)
 *   - InoLiteRtLmToolBase.h         UInoLiteRtLmToolBase (Blueprintable
 *                                    tool base class)
 *   - InoLiteRtLmAddNumbersTool.h   canonical tool sample
 *
 * Settings live in Public/InoLiteRtLmSettings.h (a separate
 * UDeveloperSettings page from the InoAgents one).
 */
class FInoLiteRtLmModule : public IModuleInterface
{
public:
    //~ IModuleInterface
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    //~ End of IModuleInterface
};
