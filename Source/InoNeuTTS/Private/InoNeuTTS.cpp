// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTTS.h"

#include "InoAgentsLog.h"  // shared LogInoAgents category

#include "Modules/ModuleManager.h"

void FInoNeuTTSModule::StartupModule()
{
    // The sibling InoLiteRT plugin pre-loads both the bare LiteRT
    // (libLiteRt.dll / libLiteRtLm.so) and the LiteRT-LM
    // (LiteRtLm.dll) runtimes at LoadingPhase=PreLoadingScreen,
    // strictly before this Default-phase StartupModule runs. Nothing
    // for this module to do at startup — UInoNeuTTSSubsystem loads
    // its backbone (.litertlm) + decoder (.tflite) lazily off the
    // game thread when LoadModelAsync is called.
    UE_LOG(LogInoAgents, Verbose, TEXT("[NeuTTS] module started."));
}

void FInoNeuTTSModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FInoNeuTTSModule, InoNeuTTS)
