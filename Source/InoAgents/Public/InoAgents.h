// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Modules/ModuleManager.h"

/**
 * InoAgents runtime module.
 *
 * Hosts the cross-cutting infrastructure that the plugin's sub-modules
 * (LiteRT-LM, ElevenLabs, etc.) share — the LogInoAgents log category,
 * shared audio helpers, animation helpers, and the in-PIE chat panel.
 *
 * Foundation runtimes used by sub-modules (LiteRT-LM via the `InoLiteRT`
 * plugin) are owned by sibling plugins and pre-loaded at
 * LoadingPhase=PreLoadingScreen, before this Default-phase StartupModule.
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
