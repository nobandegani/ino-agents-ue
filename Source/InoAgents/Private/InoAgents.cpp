// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoAgents.h"

#include "InoAgentsLog.h"

#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

// LiteRT-LM C API. Staged into Source/ThirdParty/InoAgentsLibrary/Public/ by
// LiteRtLm/scripts/build-win64.ps1. Used here only for the trivial
// set_min_log_level() startup smoke test. Every other public API call
// happens inside the smoke test commands under Private/SmokeTests/ or in
// the Milestone D classes under Private/LiteRtLm/ — module startup
// should stay cheap and side-effect-free.
#include "litert/lm/engine.h"

// ONNX Runtime startup glue. Matches the LiteRT-LM pattern: load the DLL
// here, run a trivial smoke test, keep anything heavier (session creation,
// actual model inference) to the subsystem / worker layer.
#include "InoOnnxModule.h"

// llama.cpp startup glue — third runtime alongside LiteRT-LM and ORT.
// Init() preloads the DLL dependency chain on Windows, resolves the
// function-pointer vtable, and calls llama_backend_init. Failure is
// non-fatal (matches ORT's treatment) — every llama.cpp consumer
// null-checks InoAgents::LlamaCpp::GetApi() before use.
#include "InoLlamaCppModule.h"

// Single definition for the shared log category declared in InoAgentsLog.h.
// Everything in this module — including every file under Private/SmokeTests/
// — logs to LogInoAgents via that header.
DEFINE_LOG_CATEGORY(LogInoAgents);

namespace
{
    /**
     * Resolve the absolute path to a runtime DLL staged alongside the plugin
     * binaries. Returns an empty string on unsupported platforms.
     */
    FString ResolveStagedDllPath(const TCHAR* DllFileName)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoAgents"));
        if (!Plugin.IsValid())
        {
            return FString();
        }

        const FString BaseDir = Plugin->GetBaseDir();

#if PLATFORM_WINDOWS
        return FPaths::Combine(BaseDir, TEXT("Binaries/ThirdParty/InoAgentsLibrary/Win64"), DllFileName);
#else
        // Phases 2-5 (Android, iOS, Linux, macOS) are not yet implemented.
        // Returning empty causes GetDllHandle() below to no-op gracefully,
        // which is the correct behavior during phase 1.
        return FString();
#endif
    }

    /**
     * Load a staged runtime DLL by filename. Logs on success and failure.
     *
     * Unlike the stock UE "Third Party Library" plugin template, this does
     * NOT show a blocking MessageDialog on failure — that dialog pops up
     * every editor start if a single DLL is missing, which is hostile during
     * development. An error log is sufficient; UInoLiteRtLmSubsystem::LoadModelAsync
     * surfaces the actual load failure to Blueprint / C++ via its
     * FOnInoLiteRtLmModelLoaded delegate.
     */
    void* LoadStagedDll(const TCHAR* DllFileName)
    {
        const FString Path = ResolveStagedDllPath(DllFileName);
        if (Path.IsEmpty())
        {
            UE_LOG(LogInoAgents, Warning,
                   TEXT("InoAgents: not loading %s (unsupported platform in phase 1)."),
                   DllFileName);
            return nullptr;
        }

        void* Handle = FPlatformProcess::GetDllHandle(*Path);
        if (Handle)
        {
            UE_LOG(LogInoAgents, Log,
                   TEXT("InoAgents: loaded %s from %s"),
                   DllFileName, *Path);
        }
        else
        {
            UE_LOG(LogInoAgents, Error,
                   TEXT("InoAgents: failed to load %s from %s. Did you run "
                        "Plugins/InoAgents/LiteRtLm/scripts/build-win64.ps1?"),
                   DllFileName, *Path);
        }
        return Handle;
    }
}

void FInoAgentsModule::StartupModule()
{
#if PLATFORM_WINDOWS
    // Load order is critical — each DLL must be in memory before any
    // DLL that imports from it:
    //
    //   1. libGemmaModelConstraintProvider.dll (standalone, no deps on others)
    //   2. libLiteRt.dll                       (LiteRT core runtime)
    //   3. LiteRtLm.dll                        (imports from libLiteRt.dll)
    //   4. libLiteRtWebGpuAccelerator.dll      (imports from libLiteRt.dll)
    //   5. libLiteRtTopKWebGpuSampler.dll      (imports from libLiteRt.dll)
    //
    // With --define=litert_link_capi_so=true, LiteRtLm.dll dynamically
    // links against libLiteRt.dll (instead of statically including it),
    // so libLiteRt.dll MUST be loaded before LiteRtLm.dll or Windows
    // will fail with "Missing import: libLiteRt.dll" (GetLastError=126).
    GemmaConstraintProviderHandle = LoadStagedDll(TEXT("libGemmaModelConstraintProvider.dll"));
    LiteRtHandle                  = LoadStagedDll(TEXT("libLiteRt.dll"));
    LiteRtLmHandle                = LoadStagedDll(TEXT("LiteRtLm.dll"));

    // GPU accelerator DLLs — pre-load them so LiteRT's engine can
    // find them when it internally calls LoadLibraryA by filename.
    if (LiteRtHandle)
    {
        WebGpuAcceleratorHandle  = LoadStagedDll(TEXT("libLiteRtWebGpuAccelerator.dll"));
        TopKWebGpuSamplerHandle = LoadStagedDll(TEXT("libLiteRtTopKWebGpuSampler.dll"));
    }

    const bool bCanCallLiteRtLm = (LiteRtLmHandle != nullptr);
#elif PLATFORM_ANDROID
    // On Android, the .so files are loaded automatically by Android's
    // dynamic linker at process startup. Our game's main library
    // (libUnreal.so) links against libLiteRtLm.so via
    // PublicAdditionalLibraries in InoAgentsLibrary.Build.cs, so the
    // symbols resolve through the normal Android linker path — no
    // FPlatformProcess::GetDllHandle() calls needed. The UPL XML's
    // <soLoadLibrary> additionally asks the GameActivity Java side to
    // preload the libs (harmless but redundant given the DT_NEEDED
    // walk the linker already performs).
    //
    // The GPU accelerator .so files are also in the APK's lib/arm64-v8a/
    // (via the <resourceCopies> directive in the UPL XML). When the
    // LiteRT engine calls dlopen("libLiteRtWebGpuAccelerator.so")
    // internally, Android's linker finds them in the standard library
    // search path for the process.
    const bool bCanCallLiteRtLm = true;
#else
    // iOS / Linux / macOS: stubs in InoLiteRtLmStubs_NonWindows.cpp
    // provide no-op implementations. Everything is linkable but every
    // call returns nullptr / fails gracefully.
    const bool bCanCallLiteRtLm = true;
#endif

    // --------------------------------------------------------------
    // Trivial startup smoke test: call one cheap C API function so we
    // know the LiteRT-LM library is loaded and callable. On Windows
    // this proves the delay-load trampolines and import lib wiring.
    // On Android this proves the UPL-driven loadLibrary calls worked.
    // On stub platforms this is a no-op (the stub returns without doing
    // anything) which still proves the build linked successfully.
    //
    // The more substantial smoke tests (LoadEngineTest, GenerateTest,
    // ConversationTest, ToolCallTest, StreamTest) are registered as
    // console commands from files under Private/SmokeTests/.
    // --------------------------------------------------------------
    if (bCanCallLiteRtLm)
    {
        litert_lm_set_min_log_level(0);
        UE_LOG(LogInoAgents, Log,
               TEXT("InoAgents: smoke test passed — litert_lm_set_min_log_level(0) returned cleanly."));
    }

    // --------------------------------------------------------------
    // ONNX Runtime startup. Same shape as the LiteRT-LM block above:
    // Init() handles per-platform DLL/.so loading (Windows needs an
    // explicit GetDllHandle because InoOnnxRuntime.Build.cs uses
    // PublicDelayLoadDLLs; Android leaves the .so to the dynamic
    // linker), and internally runs a trivial smoke test
    // (OrtApi::GetAvailableProviders) so we see in the log whether
    // ORT is callable end-to-end.
    //
    // Failure here is non-fatal — Init() logs its own error and
    // returns nullptr. Any future subsystem that actually uses ORT
    // (e.g. the Chatterbox Turbo TTS worker in a follow-up phase)
    // should surface a user-visible error via its own OnLoaded /
    // OnError delegate instead of relying on module-startup state.
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
    // returns false. Any llama.cpp consumer (future subsystem,
    // conversation, smoke tests) null-checks
    // InoAgents::LlamaCpp::GetApi() before calling into the vtable,
    // so a failed init surfaces as a no-op rather than a crash.
    // --------------------------------------------------------------
    InoAgents::LlamaCpp::Init();
}

void FInoAgentsModule::ShutdownModule()
{
    // Mirror-image teardown: llama.cpp first (opened last), then ORT,
    // then LiteRT-LM. All are independent subsystems — order only
    // matters for keeping the log readable.
    InoAgents::LlamaCpp::Shutdown();

    // Shut down ONNX Runtime before LiteRT-LM — independent subsystems,
    // but keeping teardown in mirror-image of startup is a cheap habit
    // and leaves the log easier to read if something goes wrong.
    InoAgents::Onnx::Shutdown(OnnxRuntimeHandle);
    OnnxRuntimeHandle = nullptr;

#if PLATFORM_WINDOWS
    // Unload in reverse dependency order: GPU DLLs first (they import
    // from libLiteRt), then libLiteRt, then our main DLL, then the
    // constraint provider.
    if (TopKWebGpuSamplerHandle)
    {
        FPlatformProcess::FreeDllHandle(TopKWebGpuSamplerHandle);
        TopKWebGpuSamplerHandle = nullptr;
    }
    if (WebGpuAcceleratorHandle)
    {
        FPlatformProcess::FreeDllHandle(WebGpuAcceleratorHandle);
        WebGpuAcceleratorHandle = nullptr;
    }
    if (LiteRtHandle)
    {
        FPlatformProcess::FreeDllHandle(LiteRtHandle);
        LiteRtHandle = nullptr;
    }
    if (LiteRtLmHandle)
    {
        FPlatformProcess::FreeDllHandle(LiteRtLmHandle);
        LiteRtLmHandle = nullptr;
    }
    if (GemmaConstraintProviderHandle)
    {
        FPlatformProcess::FreeDllHandle(GemmaConstraintProviderHandle);
        GemmaConstraintProviderHandle = nullptr;
    }
#endif  // PLATFORM_WINDOWS

    // On Android / iOS / Linux / macOS: nothing to unload — the shared
    // libraries are managed by the OS dynamic linker and freed at
    // process exit along with the rest of the game process.
}

IMPLEMENT_MODULE(FInoAgentsModule, InoAgents)
