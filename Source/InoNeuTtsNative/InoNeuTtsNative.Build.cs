// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

/// <summary>
/// NeuTTS Nano TTS — Neuphonic's Qwen2-derived ~117M-param GGUF backbone
/// + NeuCodec ONNX decoder. Sub-module of the InoAgents plugin, sibling
/// to InoChatterboxOnnx.
///
/// Self-contained deletion unit: rm this folder + drop the entry from
/// InoAgents.uplugin's Modules array and the entire NeuTTS Nano impl
/// goes with it (subsystem, runner, worker, voice registry, smoke tests,
/// settings page).
/// </summary>
public class InoNeuTtsNative : ModuleRules
{
	public InoNeuTtsNative(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Disable unity build for this module: same reason as
		// InoChatterboxOnnx — this small module's .cpps are likelier to
		// hit anonymous-namespace name collisions when merged into one
		// unity TU than they were when buried among the InoAgents core
		// files. Build-time hit is small (~10 .cpps).
		bUseUnity = false;

		PublicIncludePaths.AddRange(
			new string[] {
			}
			);

		PrivateIncludePaths.AddRange(
			new string[] {
				// Subdirectory of Private/ that holds the runner + worker
				// + voice registry + prompt builder. .cpps include each
				// other with bare names (e.g. #include "InoNeuTtsNanoRunner.h").
				Path.Combine(ModuleDirectory, "Private", "NeuTtsNano"),

				// Subdirectory of Private/ that holds the sub-module's own
				// smoke tests.
				Path.Combine(ModuleDirectory, "Private", "SmokeTests"),
			}
			);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",       // UCLASS / USTRUCT / UENUM macros — used in
				                     // Public/ for UInoNeuTtsNanoSubsystem etc.
				"Engine",            // UGameInstanceSubsystem.

				"InoAgents",         // For the shared LogInoAgents category +
				                     // InoSmokeTestCommon helper +
				                     // UInoAudioFunctionLibrary (mono WAV /
				                     // PCM helpers in InoAgents Public/Audio).

				"InoOnnx",           // ONNX Runtime — for FInoOnnxSession that
				                     // the NeuCodec decoder runs on.

				"InoLlama",          // llama.cpp runtime — for the Qwen2-derived
				                     // GGUF backbone via FLlamaCppApi vtable.

				"DeveloperSettings", // UDeveloperSettings base class for
				                     // UInoNeuTtsNativeSettings.

				"RuntimeAudioImporter", // UStreamingSoundWave — streaming
				                        // audio sink for NeuTTS output.

				"HTTP",              // FHttpModule for the multi-file model
				                     // download flow.
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",      // IPluginManager::Get — used by the subsystem
				                 // to resolve the plugin's base directory for
				                 // bundled-resource lookup (default voice).
				"Json",          // FJsonObject / FJsonSerializer for parsing
				                 // .nvoice.json voice files.
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
