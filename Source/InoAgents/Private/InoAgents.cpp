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
}

void FInoAgentsModule::ShutdownModule()
{
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
