// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

// Forward decl — full definition lives in litert/c/litert_common.h. Keeping
// the LiteRT include out of public-facing headers avoids dragging the C API
// surface into every TU that consumes the runner.
struct LiteRtEnvironmentT;
typedef struct LiteRtEnvironmentT* LiteRtEnvironment;

/**
 * Process-wide singleton holding one LiteRtEnvironment for the
 * InoChatterboxLiteRT module.
 *
 * Why one env per process: LiteRtEnvironment owns the accelerator registry,
 * the GPU compute environment, and shared compiler-plugin state. Creating
 * one per model would be wasteful and (per the LiteRT C API headers) is the
 * non-idiomatic path. Multiple CompiledModels can share a single env.
 *
 * Lifetime: lazily created on first Get() call (typically when the first
 * .tflite is loaded — which happens off the game thread, so the lazy-init
 * cost doesn't show up on the editor's main loop). Torn down explicitly by
 * Shutdown() from FInoChatterboxLiteRTModule::ShutdownModule, which runs
 * BEFORE the sibling InoLiteRT plugin unloads its DLLs (InoLiteRT loads at
 * PreLoadingScreen + unloads in reverse module-load order, so consumer
 * modules' ShutdownModule run first).
 *
 * Phase 1: env is created with no options (default accelerator registry).
 * Backend selection happens per-CompiledModel via LiteRtOptions, not here.
 */
namespace InoChatterboxLiteRT
{
    /**
     * Returns the cached LiteRtEnvironment, creating it on first call.
     * Returns nullptr only if creation failed (logged at Error level once).
     * Thread-safe (FCriticalSection-guarded init).
     */
    LiteRtEnvironment GetEnvironment();

    /**
     * Destroys the cached LiteRtEnvironment if one was created. Safe to call
     * multiple times. Called from FInoChatterboxLiteRTModule::ShutdownModule.
     */
    void Shutdown();
}
