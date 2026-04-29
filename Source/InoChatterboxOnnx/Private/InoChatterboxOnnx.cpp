// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxOnnx.h"

#include "Modules/ModuleManager.h"

void FInoChatterboxOnnxModule::StartupModule()
{
    // Nothing to do at module load. The ONNX Runtime DLLs are pre-loaded
    // by the sibling InoOnnx plugin at PreLoadingScreen, so OrtApi is
    // already callable. Models are loaded lazily by
    // UInoChatterboxTtsSubsystem::LoadModelsAsync off the game thread —
    // doing it here would freeze the editor for seconds.
}

void FInoChatterboxOnnxModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FInoChatterboxOnnxModule, InoChatterboxOnnx)
