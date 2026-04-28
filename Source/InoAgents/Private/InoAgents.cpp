// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoAgents.h"

#include "InoAgentsLog.h"

#include "Modules/ModuleManager.h"

// ONNX Runtime startup glue. Init() handles per-platform DLL/.so loading
// and runs a trivial smoke test (OrtApi::GetAvailableProviders) so we see
// in the log whether ORT is callable end-to-end. The actual session creation
// and inference happen in the consumer subsystems (Chatterbox, NeuTTS Nano,
// future vision/embedding workloads).
#include "InoOnnxModule.h"

// llama.cpp startup glue — third runtime alongside LiteRT-LM (provided by
// InoLiteRT plugin) and ORT. Init() preloads the DLL dependency chain on
// Windows, resolves the function-pointer vtable, and calls
// llama_backend_init. Failure is non-fatal (matches ORT's treatment) —
// every llama.cpp consumer null-checks InoAgents::LlamaCpp::GetApi() before use.
#include "InoLlamaCppModule.h"

// Single definition for the shared log category declared in InoAgentsLog.h.
// Everything in this module — including every file under Private/SmokeTests/
// — logs to LogInoAgents via that header.
DEFINE_LOG_CATEGORY(LogInoAgents);

void FInoAgentsModule::StartupModule()
{
    // --------------------------------------------------------------
    // LiteRT + LiteRT-LM: NOT loaded here.
    //
    // The separate `InoLiteRT` plugin owns the LiteRT and LiteRT-LM
    // runtimes. It pre-loads the DLL chain (libGemmaModelConstraintProvider,
    // libLiteRt, LiteRtLm, GPU accelerators) at LoadingPhase=PreLoadingScreen,
    // which runs strictly before this module's Default-phase StartupModule.
    // By the time we get here, both runtimes are already mapped into the
    // process and callable — calling FindPlugin("InoLiteRT") would just
    // confirm what's already true.
    //
    // InoAgents.uplugin declares "InoLiteRT" in its "Plugins" array,
    // so UE refuses to load InoAgents without InoLiteRT also being present
    // and enabled. The InoAgents.Build.cs PublicDependencyModuleNames entry
    // for "InoLiteRT" pulls in the headers + import libs at compile time.
    // --------------------------------------------------------------

    // --------------------------------------------------------------
    // ONNX Runtime startup.
    //
    // Init() handles per-platform DLL/.so loading (Windows needs an
    // explicit GetDllHandle because InoOnnxRuntime.Build.cs uses
    // PublicDelayLoadDLLs; Android leaves the .so to the dynamic
    // linker), and internally runs a trivial smoke test
    // (OrtApi::GetAvailableProviders) so we see in the log whether
    // ORT is callable end-to-end.
    //
    // Failure here is non-fatal — Init() logs its own error and
    // returns nullptr. Any subsystem that actually uses ORT (Chatterbox
    // Turbo TTS, NeuTTS Nano's NeuCodec decoder) surfaces a user-visible
    // error via its own OnLoaded / OnError delegate instead of relying
    // on module-startup state.
    // --------------------------------------------------------------
    OnnxRuntimeHandle = InoAgents::Onnx::Init();

    // --------------------------------------------------------------
    // llama.cpp startup. Same shape as ORT above: Init() handles
    // per-platform DLL / .so loading (Windows preloads the ggml
    // dependency chain + vulkan backend; Android relies on the UPL's
    // <soLoadLibrary> to have already mapped libllama.so + cascaded
    // DT_NEEDED), resolves a function-pointer vtable, registers all
    // ggml backends (CPU variants + Vulkan), and calls
    // llama_backend_init. Summary line including the build + system
    // info is emitted to LogInoAgents on success.
    //
    // Failure here is non-fatal — Init() logs its own error and
    // returns false. Any llama.cpp consumer (NeuTTS Nano subsystem,
    // future GGUF consumers, smoke tests) null-checks
    // InoAgents::LlamaCpp::GetApi() before calling into the vtable,
    // so a failed init surfaces as a no-op rather than a crash.
    // --------------------------------------------------------------
    InoAgents::LlamaCpp::Init();
}

void FInoAgentsModule::ShutdownModule()
{
    // Mirror-image teardown: llama.cpp first (opened last), then ORT.
    // LiteRT / LiteRT-LM are owned by the InoLiteRT plugin — its
    // ShutdownModule frees those DLL handles independently.
    InoAgents::LlamaCpp::Shutdown();

    InoAgents::Onnx::Shutdown(OnnxRuntimeHandle);
    OnnxRuntimeHandle = nullptr;
}

IMPLEMENT_MODULE(FInoAgentsModule, InoAgents)
