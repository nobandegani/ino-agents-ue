// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoAgents.h"

#include "InoAgentsLog.h"

#include "Modules/ModuleManager.h"

// Single definition for the shared log category declared in InoAgentsLog.h.
// Everything in this module — including every file under Private/SmokeTests/
// — logs to LogInoAgents via that header.
DEFINE_LOG_CATEGORY(LogInoAgents);

void FInoAgentsModule::StartupModule()
{
    // Sub-module foundation runtimes (LiteRT-LM via the `InoLiteRT`
    // plugin) are owned by sibling plugins which pre-load at
    // LoadingPhase=PreLoadingScreen, strictly before this Default-phase
    // StartupModule. Nothing for this module to do here — every consumer
    // subsystem loads its own model lazily off the game thread.
}

void FInoAgentsModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FInoAgentsModule, InoAgents)
