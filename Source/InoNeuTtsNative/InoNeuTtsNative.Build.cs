// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

/// <summary>
/// NeuTTS Nano + NeuTTS Air — Neuphonic's Qwen2-derived GGUF backbones
/// (~120M Nano / ~360M Air active params) + NeuCodec ONNX decoder.
/// Sub-module of the InoAgents plugin.
///
/// Self-contained deletion unit: rm this folder + drop the entry from
/// InoAgents.uplugin's Modules array and the entire NeuTTS impl goes
/// with it (subsystem, runner, worker, voice registry, smoke tests).
/// </summary>
public class InoNeuTtsNative : ModuleRules
{
	public InoNeuTtsNative(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Disable unity build for this module: same reason as
		// InoChatterboxNative — small module, .cpps include narrow third-
		// party C headers (llama.h, onnxruntime_c_api.h) that don't blend
		// well with unity TUs and risk anonymous-namespace name collisions.
		bUseUnity = false;

		PrivateIncludePaths.AddRange(new string[]
		{
			// .cpps under NeuTtsNano/ and SmokeTests/ include each other
			// with bare names (e.g. #include "InoNeuTtsRunner.h").
			Path.Combine(ModuleDirectory, "Private"),
			Path.Combine(ModuleDirectory, "Private", "SmokeTests"),
		});

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",            // UCLASS / USTRUCT / UENUM macros — used in Public/.
			"Engine",                 // UGameInstanceSubsystem.

			"InoAgents",              // Shared LogInoAgents category +
			                          // UInoAudioFunctionLibrary helpers
			                          // (mono WAV / PCM helpers in Public/Audio).

			"InoLlama",               // llama.cpp runtime — for the Qwen2-
			                          // derived GGUF backbone via FLlamaCppApi
			                          // vtable (InoAgents::LlamaCpp::GetApi()).

			"InoOnnx",                // ONNX Runtime — for FInoOnnxSession that
			                          // the NeuCodec decoder runs on.

			"InoSpeakNG",             // espeak-ng — for runtime phonemization
			                          // (input text + reference voice ref_text).

			"RuntimeAudioImporter",   // UStreamingSoundWave — streaming audio
			                          // sink for synthesis output.
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",                   // FJsonObject / FJsonSerializer for parsing
			                          // .nvoice.json voice files.
			"JsonUtilities",
			"Projects",               // IPluginManager — used by the subsystem
			                          // to resolve the plugin's base directory
			                          // for model + voice path lookup.
		});

		DynamicallyLoadedModuleNames.AddRange(new string[]
		{
		});
	}
}
