// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

// Use the EXACT typedef form upstream's LITERT_DEFINE_HANDLE macro emits in
// C++ mode (litert_common.h:55-58):
//     typedef class LiteRtEnvironmentT* LiteRtEnvironment;
//
// Critical: must use `class`, NOT `struct LiteRtEnvironmentT;` + a separate
// typedef. MSVC otherwise mangles the type differently across TUs that see
// `struct` first (this header) vs ones that see `class` first (via
// litert_common.h directly), producing PEAU-vs-PEAV name mangling and
// link-time "unresolved external" failures on every function that takes a
// LiteRtEnvironment parameter.
typedef class LiteRtEnvironmentT* LiteRtEnvironment;

namespace InoQwen3ASRLiteRT
{
    /** Lazy-init process-singleton LiteRtEnvironment for this module. */
    LiteRtEnvironment GetEnvironment();

    /** Tear down the env. Called from ShutdownModule. */
    void Shutdown();
}
