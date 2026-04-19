// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

/// <summary>
/// External UE module that exposes Microsoft's prebuilt ONNX Runtime to
/// the InoAgents plugin.
///
/// The binaries consumed here are downloaded and staged by
///   Plugins/InoAgents/OnnxRuntime/scripts/setup-onnxruntime.ps1
/// which pulls the official Microsoft prebuilt release for the version
/// pinned in OnnxRuntime/ONNXRUNTIME_VERSION, and writes:
///
///   Source/ThirdParty/InoOnnxRuntime/
///     Public/                          C / C++ API headers
///     Win64/onnxruntime.lib            import library for MSVC link
///
///   Binaries/ThirdParty/InoOnnxRuntime/
///     Win64/onnxruntime.dll            main runtime (~13 MB)
///     Win64/onnxruntime_providers_shared.dll  shared provider interface (~20 KB)
///     Android/arm64-v8a/libonnxruntime.so     main runtime + XNNPACK (~25 MB)
///
/// Consumers do
///     #include "onnxruntime_cxx_api.h"
/// and link against Ort::Session, Ort::Env, Ort::Value, etc.
///
/// This module does NOT ship the GPU execution providers (CUDA /
/// TensorRT / DirectML). Reasons are documented in
/// OnnxRuntime/scripts/setup-onnxruntime.ps1. DirectML (for D3D12
/// GPU acceleration matching UE's renderer) is planned as a separate
/// follow-up module that consumers can optionally depend on.
/// </summary>
public class InoOnnxRuntime : ModuleRules
{
	public InoOnnxRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		// Public headers for ORT. Consumers use them as
		//     #include "onnxruntime_cxx_api.h"
		// rather than relative paths, so expose as a system include.
		PublicSystemIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// --- Import library (link time) ---
			PublicAdditionalLibraries.Add(Path.Combine(ModuleDirectory, "Win64", "onnxruntime.lib"));

			// --- Runtime DLL (delay-loaded at first use) ---
			//
			// onnxruntime.dll                       — ORT core runtime.
			//                                         Contains the CPU execution
			//                                         provider + Ort::Session / Env /
			//                                         Value machinery.
			// onnxruntime_providers_shared.dll      — shared-provider plumbing used
			//                                         by out-of-process execution
			//                                         providers (DirectML, CUDA,
			//                                         TensorRT). Ships with ORT even
			//                                         in CPU-only builds. We don't
			//                                         call into it directly for CPU
			//                                         workloads, so we do NOT delay-load
			//                                         it — only stage it alongside
			//                                         onnxruntime.dll so it is present
			//                                         if a future GPU-provider phase
			//                                         needs it.
			PublicDelayLoadDLLs.Add("onnxruntime.dll");

			// --- Runtime staging (copied next to the executable at cook/package time) ---
			RuntimeDependencies.Add("$(PluginDir)/Binaries/ThirdParty/InoOnnxRuntime/Win64/onnxruntime.dll");
			RuntimeDependencies.Add("$(PluginDir)/Binaries/ThirdParty/InoOnnxRuntime/Win64/onnxruntime_providers_shared.dll");
		}
		else if (Target.Platform == UnrealTargetPlatform.Android)
		{
			// Android arm64-v8a artifacts staged by setup-onnxruntime.ps1
			// under Binaries/ThirdParty/InoOnnxRuntime/Android/arm64-v8a/.
			// Android does NOT use import libraries — .so files are linked
			// directly with PublicAdditionalLibraries at UE build time, and
			// the linker resolves symbols against the .so's export table.
			string Arm64BinDir = Path.Combine(
				PluginDirectory, "Binaries/ThirdParty/InoOnnxRuntime/Android/arm64-v8a");

			PublicAdditionalLibraries.Add(Path.Combine(Arm64BinDir, "libonnxruntime.so"));

			// RuntimeDependencies on Android does NOT actually stage the .so
			// into the APK's lib/arm64-v8a/ directory — that only happens via
			// the UPL's <resourceCopies> <copyFile> directive. We still list
			// the .so in RuntimeDependencies so the UE packaging manifest
			// knows about it (cook-time staging into Staged/), but the UPL
			// is what gets it into the final APK. Lesson learned from the
			// LiteRT-LM integration, see InoAgentsLibrary.Build.cs for the
			// full story.
			string SoPath = Path.Combine(Arm64BinDir, "libonnxruntime.so");
			if (File.Exists(SoPath))
			{
				RuntimeDependencies.Add(SoPath);
			}

			// Apply the UPL (Unreal Plugin Language) XML that tells UE's
			// APK packager to bundle libonnxruntime.so into lib/arm64-v8a/
			// and inject the corresponding System.loadLibrary() call.
			AdditionalPropertiesForReceipt.Add(
				"AndroidPlugin",
				Path.Combine(ModuleDirectory, "InoOnnxRuntime_UPL_Android.xml"));
		}
		else
		{
			// iOS / Linux / macOS not yet implemented. Any plugin code that
			// #includes ORT headers and calls Ort::Session etc. will fail
			// to link on those platforms. If / when we extend ONNX Runtime
			// to a new platform, add a platform branch here plus the matching
			// prebuilt-binary download in setup-onnxruntime.ps1.
		}
	}
}
