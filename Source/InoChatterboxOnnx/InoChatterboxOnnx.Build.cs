// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

/// <summary>
/// Chatterbox Turbo TTS via ONNX Runtime — sub-module of the InoAgents
/// plugin. Self-contained: deletes cleanly when the implementation is
/// deprecated by removing this directory + its entries from
/// InoAgents.uplugin's Modules array and from any other module's
/// Build.cs that imports it.
///
/// The Build.cs sibling InoAgents.Build.cs deliberately does NOT depend
/// on InoChatterboxOnnx — sub-modules of the plugin are reached via
/// game code that loads the relevant subsystem on demand. Adding a
/// reverse dep here would defeat the deprecation-as-folder-delete
/// property.
/// </summary>
public class InoChatterboxOnnx : ModuleRules
{
	public InoChatterboxOnnx(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Disable unity build for this module: InoChatterboxRunner.cpp and
		// InoChatterboxTest.cpp both have anonymous-namespace constants
		// (kStartSpeechToken, ApplyRepetitionPenalty, etc.) that clash when
		// merged into one unity TU. Pre-split they ended up in separate
		// Module.InoAgents.N.cpp groups so they didn't collide; in this
		// smaller module they always end up in the same group. Build-time
		// hit is small (~15 .cpps).
		bUseUnity = false;

		PublicIncludePaths.AddRange(
			new string[] {
			}
			);

		PrivateIncludePaths.AddRange(
			new string[] {
				// Subdirectory of Private/ that holds the Chatterbox runner
				// + workers + tokenizer + audio I/O. The .cpps include each
				// other with bare names (e.g. #include "InoChatterboxRunner.h").
				Path.Combine(ModuleDirectory, "Private", "Chatterbox"),

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
				                    // Public/ for FInoChatterboxVoice etc.
				"Engine",           // UGameInstanceSubsystem — UInoChatterboxTtsSubsystem.

				"InoAgents",        // For the shared LogInoAgents category +
				                    // InoSmokeTestCommon helper. The Chatterbox
				                    // subsystem reads its OWN settings, so no
				                    // dep on InoAgents Settings is needed.

				"InoOnnx",          // ONNX Runtime — the entire reason this
				                    // module exists. Provides
				                    // InoAgents::Onnx::GetApi() + the OrtApi*
				                    // vtable used by FInoChatterboxRunner.

				"DeveloperSettings", // UDeveloperSettings base class for
				                     // UInoChatterboxOnnxSettings.

				"RuntimeAudioImporter", // UStreamingSoundWave +
				                        // ERuntimeAudioFormat / ERuntimeRAWAudioFormat —
				                        // the streaming audio sink for Chatterbox.

				"HTTP",             // FHttpModule for the multi-file model
				                    // download flow (HEAD probe + GET +
				                    // .partial staging + atomic rename).
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Json",          // FJsonObject / FJsonSerializer for parsing
				                 // tokenizer.json + config.json.
				"JsonUtilities",
			}
			);

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
			);
	}
}
