// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

DECLARE_LOG_CATEGORY_EXTERN(LogInoChatterboxLiteRT, Log, All);

/**
 * InoChatterboxLiteRT — Chatterbox Turbo TTS via LiteRT (TFLite-based runtime).
 *
 * Sibling of InoChatterboxNative (which uses ONNX). This module consumes ONLY
 * the LiteRT C API (libLiteRt.dll on Win64, monolithic libLiteRtLm.so on
 * Android with the LiteRT symbols statically linked in). No ONNX dependency.
 *
 * Loading phase: Default. The LiteRT runtime DLLs are pre-loaded at
 * PreLoadingScreen by the sibling InoLiteRT plugin, so by the time this
 * module's StartupModule runs, every LiteRt* C API symbol is callable.
 *
 * Public surface lives under Source/InoChatterboxLiteRT/Public/ChatterboxLiteRT/
 * (added incrementally — Phase 1 ships only this module class + log category;
 * the subsystem + Blueprint types land in Phase 4).
 */
class FInoChatterboxLiteRTModule : public IModuleInterface
{
public:
    //~ IModuleInterface
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    //~ End of IModuleInterface
};
