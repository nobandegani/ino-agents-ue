// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Onnx/InoOnnxTypes.h"

#include "onnxruntime_c_api.h"

/**
 * Private header shared by the InoOnnx Private/Onnx/ .cpp files. Pulls in
 * the full ORT C API (which public headers deliberately avoid) and
 * exposes a small set of helpers that every ORT-consuming .cpp in the
 * plugin uses:
 *
 *   - CheckOrtStatus       : uniform error-check-and-log helper
 *   - DtypeToOrt/OrtToDtype: EInoOnnxDtype <-> ONNXTensorElementDataType
 *   - DtypeElementSize     : bytes per element of a given dtype
 *   - ProviderToString     : debug-friendly name for a provider enum
 *   - OptLevelToOrt        : EInoOnnxGraphOptimizationLevel -> ORT enum
 *   - GetGlobalOrtEnv      : lazy-initialized per-process OrtEnv*
 *
 * Consumers obtain the OrtApi vtable via
 *     const OrtApi* Api = InoAgents::Onnx::GetApi();
 * already declared in InoOnnxModule.h. All calls go through that
 * vtable — we never call ORT C functions directly (same reason we
 * use GetDllExport on OrtGetApiBase in InoOnnxModule.cpp: zero
 * static linker dependency on the ORT library).
 */
namespace InoAgents::Onnx::Internal
{
    /**
     * If Status is non-null, log the error via LogInoAgents, copy the
     * message into *OutError (if non-null), release the OrtStatus, and
     * return false. If Status is null, return true (success). Passes
     * OpDescription through to the log as context — something like
     * TEXT("CreateSession") so the log line reads
     *     InoAgents: ORT CreateSession failed: <message>
     *
     * Must be called from any path that invokes an ORT C API function
     * returning OrtStatus*. Leaking an OrtStatus is a real memory leak
     * (ORT allocates the status struct with its own allocator).
     */
    bool CheckOrtStatus(
        OrtStatus* Status,
        const TCHAR* OpDescription,
        FString* OutError = nullptr);

    /**
     * Map between our dtype enum and ORT's. Returns
     * ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED for Undefined / unknown.
     */
    ONNXTensorElementDataType DtypeToOrt(EInoOnnxDtype Dtype);
    EInoOnnxDtype OrtToDtype(ONNXTensorElementDataType Ort);

    /** Bytes per tensor element for a given dtype. 0 for Undefined. */
    SIZE_T DtypeElementSize(EInoOnnxDtype Dtype);

    /** Human-readable name for log messages. Not the ORT registration name. */
    const TCHAR* ProviderToString(EInoOnnxProvider Provider);

    /** Translate our optimization level enum to ORT's GraphOptimizationLevel. */
    GraphOptimizationLevel OptLevelToOrt(EInoOnnxGraphOptimizationLevel Level);

    /**
     * Lazily construct the single global OrtEnv for the process. Returns
     * nullptr if ORT initialization has failed (GetApi() returned nullptr
     * upstream). Subsequent calls return the same pointer.
     *
     * Thread-safety: protected by an FCriticalSection inside the .cpp;
     * safe to call concurrently.
     *
     * Lifetime: the OrtEnv is owned by this helper and released at
     * FInoAgentsModule::ShutdownModule time (indirectly — the cached
     * pointer is cleared then). Do not call ReleaseEnv on the returned
     * pointer.
     */
    OrtEnv* GetGlobalOrtEnv();

    /** Release the lazily-constructed OrtEnv. Called from module
     *  shutdown. Subsequent GetGlobalOrtEnv() calls return nullptr. */
    void ReleaseGlobalOrtEnv();
}
