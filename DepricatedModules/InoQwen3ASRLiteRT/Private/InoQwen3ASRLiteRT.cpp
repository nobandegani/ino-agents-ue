// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoQwen3ASRLiteRT.h"

#include "Qwen3ASR/InoQwen3ASRLiteRTEnv.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogInoQwen3ASRLiteRT);

void FInoQwen3ASRLiteRTModule::StartupModule()
{
    // No work — InoLiteRT pre-loads libLiteRt.dll at PreLoadingScreen, so
    // the LiteRT C API is callable by the time we get here. Env + model
    // load happen lazily on the first LoadTest invocation.
}

void FInoQwen3ASRLiteRTModule::ShutdownModule()
{
    // Tear down the env BEFORE the InoLiteRT plugin unloads libLiteRt.dll —
    // same rationale as InoChatterboxLiteRT.
    InoQwen3ASRLiteRT::Shutdown();
}

IMPLEMENT_MODULE(FInoQwen3ASRLiteRTModule, InoQwen3ASRLiteRT)
