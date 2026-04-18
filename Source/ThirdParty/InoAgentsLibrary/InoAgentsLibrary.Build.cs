// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

/// <summary>
/// External UE module that consumes the LiteRT-LM runtime.
///
/// The artifacts referenced here are produced by
///   Plugins/InoAgents/LiteRtLm/scripts/build-win64.ps1
/// which builds LiteRT-LM from source via Bazel and stages:
///   - Win64/LiteRtLm.lib                    (import library, linked at UE build time)
///   - ../../Binaries/.../LiteRtLm.dll        (runtime DLL, delay-loaded at startup)
///   - ../../Binaries/.../libGemmaModelConstraintProvider.dll  (required sibling DLL)
///   - Public/litert/lm/engine.h             (public C API header)
///
/// None of these are tracked in git — they are build outputs. If they are
/// missing, run `Plugins/InoAgents/LiteRtLm/scripts/build-win64.ps1` first.
/// See Plugins/InoAgents/CLAUDE.md for the full build story.
/// </summary>
public class InoAgentsLibrary : ModuleRules
{
	public InoAgentsLibrary(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		// Public headers for LiteRT-LM's C API. Consumers do
		//     #include "litert/lm/engine.h"
		PublicSystemIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// --- Import library (link time) ---
			PublicAdditionalLibraries.Add(Path.Combine(ModuleDirectory, "Win64", "LiteRtLm.lib"));

			// --- Runtime DLLs (delay-loaded at startup) ---
			//
			// LiteRtLm.dll                            — our monolithic Bazel output,
			//                                           contains TFLite, XNNPACK, absl,
			//                                           protobuf, tokenizers, engine, etc.
			// libGemmaModelConstraintProvider.dll     — upstream LiteRT-LM prebuilt,
			//                                           required sibling of LiteRtLm.dll
			//                                           (Gemma-specific constraint provider
			//                                           used during generation)
			PublicDelayLoadDLLs.Add("LiteRtLm.dll");
			PublicDelayLoadDLLs.Add("libGemmaModelConstraintProvider.dll");

			// GPU accelerator DLLs (delay-loaded on demand by the LiteRT engine
			// when backend="gpu" is requested). These are upstream LiteRT-LM
			// prebuilt binaries from prebuilt/windows_x86_64/. The engine
			// dynamically loads them via LoadLibraryA at runtime — they do NOT
			// need to be loaded by our StartupModule, but they must be staged
			// alongside the other DLLs so LoadLibraryA can find them.
			PublicDelayLoadDLLs.Add("libLiteRt.dll");
			PublicDelayLoadDLLs.Add("libLiteRtWebGpuAccelerator.dll");
			PublicDelayLoadDLLs.Add("libLiteRtTopKWebGpuSampler.dll");

			// --- Runtime staging (copied next to the executable at cook/package time) ---
			RuntimeDependencies.Add("$(PluginDir)/Binaries/ThirdParty/InoAgentsLibrary/Win64/LiteRtLm.dll");
			RuntimeDependencies.Add("$(PluginDir)/Binaries/ThirdParty/InoAgentsLibrary/Win64/libGemmaModelConstraintProvider.dll");
			RuntimeDependencies.Add("$(PluginDir)/Binaries/ThirdParty/InoAgentsLibrary/Win64/libLiteRt.dll");
			RuntimeDependencies.Add("$(PluginDir)/Binaries/ThirdParty/InoAgentsLibrary/Win64/libLiteRtWebGpuAccelerator.dll");
			RuntimeDependencies.Add("$(PluginDir)/Binaries/ThirdParty/InoAgentsLibrary/Win64/libLiteRtTopKWebGpuSampler.dll");
		}
		else
		{
			// Phases 2-5 (Android, iOS, Linux, macOS) are not yet implemented.
			// The same Bazel build workspace at Plugins/InoAgents/LiteRtLm/ will
			// produce the corresponding libraries once each platform is ported.
			// For now, building the plugin for any non-Win64 platform will fail
			// with a missing-symbol link error, which is the correct behavior
			// during phase 1.
		}
	}
}
