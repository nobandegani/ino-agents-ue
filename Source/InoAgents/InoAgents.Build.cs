// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class InoAgents : ModuleRules
{
	public InoAgents(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[] {
				// Subdirectory of Public/ that holds the smoke-test helper
				// header (InoSmokeTestCommon.h). Exposed as a Public include
				// path so:
				//   - InoAgents Private/SmokeTests/*.cpp can keep their bare
				//     #include "InoSmokeTestCommon.h" without a subdir prefix.
				//   - sub-modules (e.g. InoChatterboxNative) that depend on
				//     InoAgents inherit the path and can use the same bare
				//     include for cross-module smoke-test reuse.
				//
				// NOTE: The shared audio helpers (UInoAudioFunctionLibrary —
				// mono WAV reader / writer + PCM helpers consumed by
				// InoChatterboxNative and any future TTS sub-module) live in
				// Public/Audio/ but are NOT given a bare include path here —
				// callers use the explicit "Audio/InoAudioFunctionLibrary.h"
				// path to keep their includes greppable as cross-module.
				Path.Combine(ModuleDirectory, "Public", "SmokeTests"),
			}
			);


		PrivateIncludePaths.AddRange(
			new string[] {
				// Subdirectory of Private/ that holds smoke-test impl
				// for InoAgents-core sub-systems (ElevenLabs). The shared
				// helpers (InoSmokeTestCommon.h) live in Public/SmokeTests/
				// so sub-modules can reuse them; InoAgentsLog.h is in
				// Public/ too. This entry exists for any header-only
				// helpers private to the test sources themselves.
				Path.Combine(ModuleDirectory, "Private", "SmokeTests"),

				// Subdirectory of Private/ that holds the ElevenLabs HTTP
				// backend implementation (subsystem, async actions, request
				// builders). Added so sibling .cpps can include each other's
				// private headers directly.
				Path.Combine(ModuleDirectory, "Private", "ElevenLabs"),

				// Subdirectory of Private/ that holds the audio function
				// library's private impl. Streaming-audio playback is
				// provided by the RuntimeAudioImporter plugin
				// (UStreamingSoundWave); we don't ship our own sound-wave
				// classes or dialogue-queue any more.
				Path.Combine(ModuleDirectory, "Private", "Audio"),

			}
			);


		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",       // UObject, UCLASS, UENUM, dynamic delegate macros — used throughout.
				"Engine",            // UGameInstanceSubsystem, GEngine, FWorldContext —
				                     // used by UInoElevenLabsSubsystem and the smoke
				                     // test helpers in InoSmokeTestCommon.
				"Json",              // FJsonObject / FJsonSerializer for parsing LiteRT-LM responses
				                     // and building ElevenLabs request bodies.
				"JsonUtilities",     // FJsonObjectWrapper — Blueprint-friendly JSON struct used by
				                     // UInoLiteRtLmToolBase::Execute for parsed tool arguments.
				"JsonBlueprintUtilities", // GetField/SetField/HasField Blueprint nodes for FJsonObjectWrapper.
				"Projects",          // IPluginManager for locating the plugin's base directory at runtime.
				"HTTP",              // FHttpModule / IHttpRequest / IHttpResponse — ElevenLabs backend only.
				"DeveloperSettings", // UDeveloperSettings base class — UElevenLabsSettings.
				"RuntimeAudioImporter",  // UStreamingSoundWave + ERuntimeAudioFormat / ERuntimeRAWAudioFormat —
				                     // the streaming TTS audio sink the dialogue queue feeds into.
			}
			);


		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
			}
			);


		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
