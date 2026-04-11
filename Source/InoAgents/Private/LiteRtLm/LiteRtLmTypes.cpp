// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "LiteRtLm/LiteRtLmTypes.h"

const char* LiteRtLmBackendToString(ELiteRtLmBackend Backend)
{
    switch (Backend)
    {
        case ELiteRtLmBackend::Cpu: return "cpu";
        case ELiteRtLmBackend::Gpu: return "gpu";
    }
    // Defensive fallback: if the enum gains a new value in the future and
    // this switch isn't updated, we default to CPU rather than returning
    // a dangling pointer.
    return "cpu";
}
