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
				// ... add public include paths required here ...
			}
			);


		PrivateIncludePaths.AddRange(
			new string[] {
				// Subdirectory of Private/ that holds the phase-1 smoke test
				// source files and their shared helpers. Adding it here lets
				// the test .cpp files #include "InoAgentsSmokeTestCommon.h"
				// and #include "InoAgentsLog.h" (the latter resolves via
				// Private/ which UBT already adds automatically).
				Path.Combine(ModuleDirectory, "Private", "SmokeTests"),

				// Subdirectory of Private/ that holds the LiteRT-LM backend
				// implementation (workers, tool impls, etc.). Added so that
				// LiteRtLmSubsystem.cpp can #include "LiteRtLmConversationWorker.h"
				// and similar private headers without relative paths.
				Path.Combine(ModuleDirectory, "Private", "LiteRtLm"),

				// Subdirectory of Private/ that holds the Slate chat-panel
				// implementation (private widgets, style, bridge .cpp).
				// Added so sibling .cpps can #include "InoAgentsChatStyle.h"
				// and #include "SInoAgentsMessageBubble.h" without relative paths.
				Path.Combine(ModuleDirectory, "Private", "UI", "Slate"),

				// Subdirectory of Private/ that holds the ElevenLabs HTTP
				// backend implementation (subsystem, async actions, request
				// builders). Added so sibling .cpps can include each other's
				// private headers directly.
				Path.Combine(ModuleDirectory, "Private", "ElevenLabs"),

				// Subdirectory of Private/ that holds the sound-wave
				// implementations plus the MP3 decoder wrapper. Added so
				// InoAgentsStreamingSoundWave.cpp can #include
				// "InoAgentsAudioMp3Decoder.h" without a relative path,
				// matching the convention used for every other private
				// subtree.
				Path.Combine(ModuleDirectory, "Private", "Audio"),

				// Third-party single-header libraries (currently: minimp3).
				// Included only by InoAgentsAudioMp3Decoder.cpp. Kept on its
				// own include path so the vendored directory is explicit in
				// the Build.cs file rather than buried under Private/Audio.
				Path.Combine(ModuleDirectory, "Private", "Audio", "ThirdParty"),
			}
			);


		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",       // UObject, UCLASS, UENUM, dynamic delegate macros — used throughout
				                     // Source/InoAgents/Public/LiteRtLm/ starting at Milestone D.1.
				"Engine",            // UDataAsset, UGameInstanceSubsystem, GEngine, FWorldContext —
				                     // used in ULiteRtLmModelConfig and ULiteRtLmSubsystem.
				"InoAgentsLibrary",  // LiteRT-LM C API via the staged header.
				"Json",              // FJsonObject / FJsonSerializer for parsing LiteRT-LM responses
				                     // and building ElevenLabs request bodies.
				"JsonUtilities",     // FJsonObjectWrapper — Blueprint-friendly JSON struct used by
				                     // ULiteRtLmToolBase::Execute for parsed tool arguments.
				"JsonBlueprintUtilities", // GetField/SetField/HasField Blueprint nodes for FJsonObjectWrapper.
				"Projects",          // IPluginManager for locating the plugin's base directory at runtime.
				"HTTP",              // FHttpModule / IHttpRequest / IHttpResponse — ElevenLabs backend only.
				"DeveloperSettings", // UDeveloperSettings base class — UElevenLabsSettings.
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// Slate UI for the in-PIE chat panel (SInoAgentsChatPanel and friends).
				// All Slate-using files live under Source/InoAgents/{Public,Private}/UI/Slate/.
				"Slate",
				"SlateCore",

				// UInoAgentsImportedSoundWave subclasses USoundWaveProcedural which
				// inherits IAudioProxyDataFactory from AudioExtensions.
				"AudioExtensions",

				// Audio::FResampler for sample-rate conversion inside the
				// streaming wave's RAW append path.
				"SignalProcessing",

				// FAudioCapture base class + FAudioCaptureDeviceInfo struct —
				// used by UInoAgentsCapturableSoundWave.
				"AudioCaptureCore",

				// Required for EKeys::Enter / EKeys::Escape constants used by the
				// chat input's Enter-to-send and ESC-to-dismiss handling. Forgetting
				// this gives a confusing link error rather than a header error.
				"InputCore",
			}
			);

		// Platform-specific capture backend. FAudioCapture is an abstract
		// interface — the concrete implementation differs per platform.
		// Only Windows + Mac use AudioCaptureRtAudio; iOS/Android have
		// their own backends (not wired up until those platforms are
		// supported).
		if (Target.Platform == UnrealTargetPlatform.Win64
		 || Target.Platform == UnrealTargetPlatform.Mac)
		{
			PrivateDependencyModuleNames.Add("AudioCaptureRtAudio");
		}

		// FEditorDelegates::PrePIEEnded is editor-only — used by the chat panel's
		// Show/Hide console commands to tear the panel down BEFORE the GameViewport
		// dies at PIE end. Wrapped in #if WITH_EDITOR at every call site.
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
