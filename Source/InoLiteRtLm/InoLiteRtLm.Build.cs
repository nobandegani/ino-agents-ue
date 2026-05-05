// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

/// <summary>
/// LiteRT-LM Gemma 4 inference — sub-module of the InoAgents plugin,
/// sibling to InoChatterboxNative and InoNeuTtsNative.
///
/// Self-contained deletion unit: rm this folder + drop the entry from
/// InoAgents.uplugin's Modules array and the entire LiteRT-LM impl
/// goes with it (subsystem, conversation, tool registry, smoke tests,
/// settings page).
///
/// The Build.cs sibling InoAgents.Build.cs deliberately does NOT depend
/// on InoLiteRtLm — sub-modules are reached via game code that fetches
/// the relevant subsystem on demand. Adding a reverse dep here would
/// defeat the deprecation-as-folder-delete property.
/// </summary>
public class InoLiteRtLm : ModuleRules
{
	public InoLiteRtLm(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Disable unity build for this module: anonymous-namespace
		// constants in the Phase-1 smoke tests (InoLoadEngineTest.cpp,
		// InoGenerateTest.cpp, etc.) collide when merged into a single
		// unity TU. Build-time hit is small (~20 .cpps).
		bUseUnity = false;

		PublicIncludePaths.AddRange(
			new string[] {
			}
			);

		PrivateIncludePaths.AddRange(
			new string[] {
				// Subdirectory of Private/ that holds the LiteRT-LM backend
				// implementation (workers, tool impls, SHA helper). Added
				// so InoLiteRtLmSubsystem.cpp can #include
				// "InoLiteRtLmConversationWorker.h" and similar private
				// headers without relative paths.
				Path.Combine(ModuleDirectory, "Private", "LiteRtLm"),

				// Subdirectory of Private/ that holds the sub-module's own
				// smoke tests.
				Path.Combine(ModuleDirectory, "Private", "SmokeTests"),
			}
			);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",      // UCLASS / USTRUCT / UENUM macros — used in
				                    // Public/LiteRtLm/ for FInoLiteRtLmModelConfig etc.
				"Engine",           // UGameInstanceSubsystem — UInoLiteRtLmSubsystem.

				"InoAgents",        // For the shared LogInoAgents category +
				                    // InoSmokeTestCommon helper. The LiteRT-LM
				                    // subsystem reads its OWN settings, so no
				                    // dep on UInoAgentsSettings is needed.

				"InoLiteRT",        // LiteRT + LiteRT-LM C APIs — the entire
				                    // reason this module exists. Provides
				                    //   #include "litert/c/litert_*.h"  (TFLite runtime)
				                    //   #include "litert/lm/engine.h"   (LLM runtime)
				                    // and stages the runtime DLLs/.so for
				                    // packaging at PreLoadingScreen.

				"InoNodes",         // Generic file downloader + SHA-256 helpers
				                    // (InoNodes::Download::DownloadFileAsync).
				                    // Public dep so FInoDownloadProgress is reachable
				                    // through the FInoLiteRtLmDownloadProgressDelegate
				                    // declared in InoLiteRtLmTypes.h.

				"Json",             // FJsonObject / FJsonSerializer for parsing
				                    // LiteRT-LM responses (tool calls, content parts).
				"JsonUtilities",    // FJsonObjectWrapper — Blueprint-friendly JSON
				                    // struct used by UInoLiteRtLmToolBase::Execute
				                    // for parsed tool arguments.
				"JsonBlueprintUtilities", // GetField/SetField/HasField Blueprint
				                          // nodes for FJsonObjectWrapper.

				"DeveloperSettings", // UDeveloperSettings base class for
				                     // UInoLiteRtLmSettings.
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",      // IPluginManager — used by the subsystem to
				                 // resolve the plugin's base directory for
				                 // bundled-resource lookup (legacy dev path).
				// HTTP dropped — model file downloads now flow through
				// InoNodes::Download::DownloadFileAsync, which owns the
				// FHttpModule integration (with .partial staging, atomic
				// rename, streaming SHA-256, multi-connection range,
				// retries, cancel tokens). See
				// Plugins/InoNodes/Source/InoNodes/Public/InoDownloader.h.
			}
			);

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
			);
	}
}
