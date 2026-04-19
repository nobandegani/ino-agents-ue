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
///     Public/                          C / C++ API headers (compile-time only)
///
///   Binaries/ThirdParty/InoOnnxRuntime/
///     Win64/InoOnnxRuntime.dll         main runtime, RENAMED from onnxruntime.dll (~13 MB)
///     Android/arm64-v8a/libonnxruntime.so   main runtime + XNNPACK (~25 MB)
///
/// Consumers #include "onnxruntime_c_api.h" for the type definitions
/// (OrtApi, OrtSession, OrtStatus, etc.) but do NOT call the exported
/// functions directly — all ORT calls go through the OrtApi vtable
/// returned by InoAgents::Onnx::GetApi(). See Source/InoAgents/Private/Onnx/
/// InoOnnxModule.{h,cpp} for the accessor.
///
/// Why no import library on Windows:
///   UE 5.7 ships multiple conflicting copies of "onnxruntime.dll"
///   (NNERuntimeORT plugin, RuntimeMetaHumanLipSync plugin, etc.).
///   Windows LoadLibrary caches DLLs by BASE NAME — whichever
///   onnxruntime.dll gets loaded into the process first wins, and
///   subsequent GetDllHandle calls with a different full path still
///   return the cached older-version handle. Our OrtApi::GetApi(24)
///   then returns nullptr because UE's bundled ORT is 1.19.x.
///
///   Fix: we rename our DLL to "InoOnnxRuntime.dll" (no other code
///   knows that name) and load it via GetProcAddress on the single
///   exported entry point "OrtGetApiBase". The returned OrtApi
///   vtable drives everything else — no static linker dependency on
///   the ORT export table at all, and no cache collision possible.
///
///   Android does not need the rename — only one libonnxruntime.so
///   lands in the APK and libUnreal.so's DT_NEEDED chain loads it
///   cleanly. We keep the implicit link via PublicAdditionalLibraries
///   there.
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
		//     #include "onnxruntime_c_api.h"
		// rather than relative paths, so expose as a system include.
		PublicSystemIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Windows: no implicit linking. No PublicAdditionalLibraries,
			// no PublicDelayLoadDLLs. The runtime consumer resolves
			// OrtGetApiBase via GetProcAddress on the renamed
			// "InoOnnxRuntime.dll" — see InoOnnxModule.cpp.
			//
			// We still need RuntimeDependencies so UE's packaging step
			// copies the DLL to the staged output alongside the game
			// executable. Without it, the shipped build would ship
			// without the ORT runtime and every session-creation call
			// would fail.
			RuntimeDependencies.Add("$(PluginDir)/Binaries/ThirdParty/InoOnnxRuntime/Win64/InoOnnxRuntime.dll");
		}
		else if (Target.Platform == UnrealTargetPlatform.Android)
		{
			// Android arm64-v8a artifacts staged by setup-onnxruntime.ps1
			// under Binaries/ThirdParty/InoOnnxRuntime/Android/arm64-v8a/,
			// RENAMED from libonnxruntime.so to libInoOnnxRuntime.so.
			//
			// Unlike Phase 2's original draft, we do NOT use
			// PublicAdditionalLibraries on Android. Rationale:
			//
			// UE 5.7 ships a Marketplace plugin (RuntimeM558be8d6854bV8,
			// the RuntimeMetaHumanLipSync variant) that distributes its
			// own libonnxruntime.so for Android arm64 at ORT v1.19.2.
			// If it's enabled in the project, UBT adds THAT .so to the
			// link path for libUnreal.so. When our C++ code references
			// OrtGetApiBase, clang's linker resolves against whichever
			// libonnxruntime.so it sees first — often the 1.19.2 one —
			// and records a versioned symbol reference
			//     U OrtGetApiBase@VERS_1.19.2
			// in libUnreal.so. At runtime the Android dynamic linker
			// can't satisfy that version against OUR 1.24.3 .so and
			// aborts the process before UE's logger is up. No stack
			// trace, no .log file, same silent-death as the Windows
			// NNE DLL-cache collision we hit in Phase 3.
			//
			// Fix: isolate. Rename our .so, drop it from the link
			// command entirely, and dlopen + dlsym at runtime. The
			// marketplace plugin's ORT (if any) stays in libUnreal.so's
			// DT_NEEDED chain for THEIR consumers; ours never touches
			// the link line. Full isolation.
			string Arm64BinDir = Path.Combine(
				PluginDirectory, "Binaries/ThirdParty/InoOnnxRuntime/Android/arm64-v8a");

			// RuntimeDependencies keeps the .so in the UE packaging
			// manifest (cook-time staging into Saved/). The actual
			// APK inclusion happens via the UPL XML's <resourceCopies>.
			string SoPath = Path.Combine(Arm64BinDir, "libInoOnnxRuntime.so");
			if (File.Exists(SoPath))
			{
				RuntimeDependencies.Add(SoPath);
			}

			// Apply the UPL (Unreal Plugin Language) XML that tells UE's
			// APK packager to copy libInoOnnxRuntime.so into lib/arm64-v8a/
			// and emit a System.loadLibrary("InoOnnxRuntime") call so the
			// .so is resident by the time InoOnnxModule::Init runs.
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
