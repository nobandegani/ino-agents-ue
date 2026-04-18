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
		else if (Target.Platform == UnrealTargetPlatform.Android)
		{
			// Android arm64-v8a artifacts produced by
			//   Plugins/InoAgents/LiteRtLm/scripts/build-android-arm64.ps1
			// The Bazel-built libLiteRtLm.so + libLiteRt.so + prebuilt GPU
			// accelerator .so files are staged into
			//   Binaries/ThirdParty/InoAgentsLibrary/Android/arm64-v8a/
			// UE's Android packaging picks them up via RuntimeDependencies
			// below and copies them into the APK's lib/arm64-v8a/ directory,
			// where Android's dynamic linker finds them at process startup.
			//
			// Unlike Windows, Android does NOT use import libraries — .so
			// files are linked directly with PublicAdditionalLibraries at
			// UE build time, and the linker resolves symbols against the
			// .so's export table.
			string Arm64BinDir = Path.Combine(
				PluginDirectory, "Binaries/ThirdParty/InoAgentsLibrary/Android/arm64-v8a");

			// Link against our Bazel output. PublicAdditionalLibraries with
			// a .so path tells UBT to add it to the linker command line.
			PublicAdditionalLibraries.Add(Path.Combine(Arm64BinDir, "libLiteRtLm.so"));

			// RuntimeDependencies with StageAsReferenceFromBinaryDir tells
			// the Android packaging step to include each .so in the APK's
			// lib/arm64-v8a/ directory. Without this, the .so files never
			// make it into the APK and the app crashes at launch with
			// "library libLiteRtLm.so not found".
			// NOTE: no libLiteRt.so on Android. Unlike Windows where
			// litert_link_capi_so=true splits LiteRT core into a
			// separate libLiteRt.dll, upstream's build:android forces
			// --dynamic_mode=off which link-statics everything into
			// libLiteRtLm.so. That makes CPU backend work (all
			// litert_lm_* symbols in a single .so) but means the
			// prebuilt GPU accelerator .so files can't resolve their
			// DT_NEEDED(libLiteRt.so) at runtime. backend=gpu is
			// currently non-functional on Android until upstream
			// ships proper Android GPU artifacts or we rebuild them.
			string[] AndroidRuntimeSoFiles = new string[]
			{
				"libLiteRtLm.so",                      // Bazel-built (monolithic ~49 MB)
				"libGemmaModelConstraintProvider.so",  // prebuilt, constraint provider
				"libLiteRtGpuAccelerator.so",          // prebuilt, general GPU accelerator
				"libLiteRtOpenClAccelerator.so",       // prebuilt, OpenCL-specific
				"libLiteRtTopKOpenClSampler.so",       // prebuilt, OpenCL top-K sampler
				"libLiteRtTopKWebGpuSampler.so",       // prebuilt, WebGPU top-K sampler
				"libLiteRtWebGpuAccelerator.so",       // prebuilt, WebGPU accelerator
			};
			foreach (string So in AndroidRuntimeSoFiles)
			{
				string SoPath = Path.Combine(Arm64BinDir, So);
				if (File.Exists(SoPath))
				{
					RuntimeDependencies.Add(SoPath);
				}
			}

			// Apply the AndroidManifest.xml additions + build.gradle tweaks
			// that tell UE's APK packager to bundle our native libraries.
			// This is the mechanism Unreal uses to inject per-plugin Android
			// build customization — we ship a UPL (Unreal Plugin Language)
			// XML file next to this Build.cs that declares the native libs
			// and, if needed, any extra JNI_OnLoad hooks.
			AdditionalPropertiesForReceipt.Add(
				"AndroidPlugin",
				Path.Combine(ModuleDirectory, "InoAgentsLibrary_UPL_Android.xml"));
		}
		else
		{
			// iOS / Linux / macOS not yet implemented. Building for those
			// platforms falls through to the stub file at
			// Source/InoAgents/Private/LiteRtLm/InoLiteRtLmStubs_NonWindows.cpp
			// which satisfies the linker with no-op implementations. Every
			// LiteRT-LM call will gracefully return nullptr / failure at
			// runtime. All other plugin features (ElevenLabs, streaming
			// audio, chat panel) continue to work.
		}
	}
}
