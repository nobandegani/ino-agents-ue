// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoNeuTtsNative.h"

#include "Modules/ModuleManager.h"

void FInoNeuTtsNativeModule::StartupModule()
{
    // Nothing to do at module load. The llama.cpp + ONNX Runtime DLLs
    // are pre-loaded by sibling plugins (InoLlama, InoOnnx) at
    // PreLoadingScreen, so both vtables are already callable. Models
    // load lazily off the game thread via
    // UInoNeuTtsNanoSubsystem::LoadModelAsync.
}

void FInoNeuTtsNativeModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FInoNeuTtsNativeModule, InoNeuTtsNative)
