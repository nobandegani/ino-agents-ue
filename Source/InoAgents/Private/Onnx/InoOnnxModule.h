// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

/**
 * ONNX Runtime module startup / shutdown glue.
 *
 * Called from FInoAgentsModule::StartupModule and ::ShutdownModule to
 * bring the ORT runtime up alongside LiteRT-LM. Keeps the ORT-specific
 * logic out of InoAgents.cpp so the module root stays focused on the
 * platform-agnostic wiring.
 *
 * Init() on Windows:
 *   - Resolves the staged path of Binaries/ThirdParty/InoOnnxRuntime/Win64/onnxruntime.dll
 *   - Calls FPlatformProcess::GetDllHandle on it (forces the DLL into the
 *     process before any Ort* symbol is referenced — required because
 *     InoOnnxRuntime.Build.cs uses PublicDelayLoadDLLs for onnxruntime.dll)
 *   - Calls OrtApi::GetAvailableProviders as a smoke test
 *
 * Init() on Android:
 *   - No explicit DLL load — libonnxruntime.so is already resident via
 *     libUnreal.so's DT_NEEDED + the UPL <soLoadLibrary> preload
 *   - Still runs the OrtApi::GetAvailableProviders smoke test
 *
 * Init() on iOS / Linux / macOS:
 *   - Warns and returns nullptr. Calls into Ort* will fail to link until
 *     those platforms are added to InoOnnxRuntime.Build.cs.
 *
 * Returns a DLL handle on Windows (the caller should hand it back to
 * Shutdown for FreeDllHandle) or nullptr on any other outcome.
 *
 * Shutdown() is the inverse — FreeDllHandle on Windows, no-op elsewhere.
 * Idempotent: nullptr handles are fine.
 */
namespace InoAgents::Onnx
{
    void* Init();
    void  Shutdown(void* Handle);
}
