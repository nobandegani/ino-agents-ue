// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoChatterboxLiteRT.h"

#include "ChatterboxLiteRT/InoChatterboxLiteRTEnv.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogInoChatterboxLiteRT);

void FInoChatterboxLiteRTModule::StartupModule()
{
    // Nothing to do at module load. The LiteRT DLLs (libLiteRt.dll on Win64;
    // monolithic libLiteRtLm.so on Android) are pre-loaded by the sibling
    // InoLiteRT plugin at PreLoadingScreen, so every LiteRt* C API symbol is
    // already callable. Models + LiteRtEnvironment are created lazily on the
    // first synthesis-subsystem call (Phase 4) — doing it here would freeze
    // the editor for seconds.
}

void FInoChatterboxLiteRTModule::ShutdownModule()
{
    // Tear down the LiteRtEnvironment singleton BEFORE the sibling InoLiteRT
    // plugin's ShutdownModule unloads libLiteRt.dll — otherwise destruction
    // of the env would call into freed code. UE unloads modules in reverse
    // load order, and InoLiteRT loaded first (PreLoadingScreen vs Default),
    // so this Shutdown() runs while libLiteRt.dll is still mapped.
    InoChatterboxLiteRT::Shutdown();
}

IMPLEMENT_MODULE(FInoChatterboxLiteRTModule, InoChatterboxLiteRT)
