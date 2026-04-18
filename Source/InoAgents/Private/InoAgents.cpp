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
    // LiteRtLm.dll depends on libGemmaModelConstraintProvider.dll at runtime.
    // Load the constraint provider FIRST so it is already resolved in memory
    // when Windows processes LiteRtLm.dll's import table.
    GemmaConstraintProviderHandle = LoadStagedDll(TEXT("libGemmaModelConstraintProvider.dll"));
    LiteRtLmHandle                = LoadStagedDll(TEXT("LiteRtLm.dll"));

    // GPU accelerator DLLs — pre-load them so LiteRT's engine can find
    // them when it internally calls LoadLibraryA("libLiteRtWebGpu...dll").
    // Without pre-loading, LoadLibraryA searches relative to the
    // process executable (UE's Engine/Binaries/Win64/) and fails
    // because the DLLs live in our plugin directory instead.
    //
    // Load order matters: libLiteRt.dll first (the GPU DLLs import
    // from it), then the accelerator and sampler. If any are missing
    // (e.g. the user didn't run build-win64.ps1 with prebuilt staging),
    // we just log a warning and skip — CPU backend still works fine.
    //
    // Unlike the core DLLs above, these use a soft-load pattern:
    // failure is not an error, just a "GPU not available" warning.
    LiteRtHandle = LoadStagedDll(TEXT("libLiteRt.dll"));
    if (LiteRtHandle)
    {
        WebGpuAcceleratorHandle  = LoadStagedDll(TEXT("libLiteRtWebGpuAccelerator.dll"));
        TopKWebGpuSamplerHandle = LoadStagedDll(TEXT("libLiteRtTopKWebGpuSampler.dll"));
    }

    // --------------------------------------------------------------
    // Trivial startup smoke test: call one cheap C API function so we know
    // that
    //   (a) InoAgentsLibrary.Build.cs's import lib is wired correctly,
    //   (b) the /EXPORT: workaround actually produces a callable symbol,
    //   (c) delay-load trampolines resolve on first call without crashing,
    //   (d) UE -> LiteRT-LM calling convention works end-to-end.
    //
    // litert_lm_set_min_log_level is the cheapest function in the public
    // API: no state, no allocation, no model file required. It just forwards
    // to absl::SetMinLogLevel. Arg 0 = INFO (no change in log verbosity).
    //
    // If this crashes, stop here — everything downstream depends on DLL
    // calls working.
    //
    // The more substantial smoke tests (LoadEngineTest, GenerateTest,
    // ConversationTest, ToolCallTest, StreamTest) are registered as console
    // commands from files under Private/SmokeTests/ and run on demand.
    // --------------------------------------------------------------
    if (LiteRtLmHandle)
    {
        litert_lm_set_min_log_level(0);
        UE_LOG(LogInoAgents, Log,
               TEXT("InoAgents: smoke test passed — litert_lm_set_min_log_level(0) returned cleanly."));
    }
}

void FInoAgentsModule::ShutdownModule()
{
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
}

IMPLEMENT_MODULE(FInoAgentsModule, InoAgents)
