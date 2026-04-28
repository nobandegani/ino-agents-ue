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
    // Foundation runtimes (LiteRT + LiteRT-LM, ONNX Runtime, llama.cpp)
    // are owned by sibling plugins (InoLiteRT, InoOnnx, InoLlama) which
    // pre-load at LoadingPhase=PreLoadingScreen, strictly before this
    // module's Default-phase StartupModule. By the time we get here, all
    // three runtimes are mapped into the process and callable.
    //
    // InoAgents.uplugin declares the three plugins in its "Plugins"
    // array, so UE refuses to load InoAgents without them also being
    // present and enabled. The InoAgents.Build.cs PublicDependencyModuleNames
    // entries for "InoLiteRT", "InoOnnx", and "InoLlama" pull in the
    // headers + import libs at compile time.
    //
    // Nothing for this module to do here — every consumer subsystem loads
    // its own model lazily off the game thread.
}

void FInoAgentsModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FInoAgentsModule, InoAgents)
