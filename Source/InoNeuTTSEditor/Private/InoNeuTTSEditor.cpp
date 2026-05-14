// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTTSEditor.h"

#include "InoAgentsLog.h"  // shared LogInoAgents

#include "Modules/ModuleManager.h"

void FInoNeuTTSEditorModule::StartupModule()
{
    // UInoNeuTTSVoiceFactory auto-registers via the UCLASS macro — UE's
    // editor walks every UFactory subclass at startup and routes
    // imports by their Formats array. No explicit AssetTools::Get()
    // call needed here.
    UE_LOG(LogInoAgents, Verbose, TEXT("[NeuTTSEditor] module started."));
}

void FInoNeuTTSEditorModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FInoNeuTTSEditorModule, InoNeuTTSEditor)
