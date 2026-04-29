// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoLiteRtLm.h"

#include "Modules/ModuleManager.h"

void FInoLiteRtLmModule::StartupModule()
{
    // Nothing to do at module load. The LiteRT-LM C API DLLs are
    // pre-loaded by the sibling InoLiteRT plugin at PreLoadingScreen,
    // so the C API is already callable. Models are loaded lazily by
    // UInoLiteRtLmSubsystem::LoadModelAsync off the game thread —
    // doing it here would freeze the editor for seconds (Gemma 4 E2B
    // is ~2.6 GB).
}

void FInoLiteRtLmModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FInoLiteRtLmModule, InoLiteRtLm)
