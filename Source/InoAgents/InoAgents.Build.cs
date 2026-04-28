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
				// the test .cpp files #include "InoSmokeTestCommon.h"
				// and #include "InoAgentsLog.h" (the latter resolves via
				// Private/ which UBT already adds automatically).
				Path.Combine(ModuleDirectory, "Private", "SmokeTests"),

				// Subdirectory of Private/ that holds the LiteRT-LM backend
				// implementation (workers, tool impls, etc.). Added so that
				// InoLiteRtLmSubsystem.cpp can #include "InoLiteRtLmConversationWorker.h"
				// and similar private headers without relative paths.
				Path.Combine(ModuleDirectory, "Private", "LiteRtLm"),

				// Subdirectory of Private/ that holds the ONNX Runtime
				// module startup glue + future ORT-consuming code (session
				// wrapper, TTS workers). Added so InoAgents.cpp can
				// #include "InoOnnxModule.h" without relative paths.
				Path.Combine(ModuleDirectory, "Private", "Onnx"),

				// Subdirectory of Private/ that holds the llama.cpp module
				// startup glue (dynamic DLL loading + function-pointer
				// vtable) and future GGUF-consuming code (subsystem,
				// conversation, worker). Added so InoAgents.cpp can
				// #include "InoLlamaCppModule.h" without relative paths.
				Path.Combine(ModuleDirectory, "Private", "LlamaCpp"),

				// Subdirectory of Private/ that holds the Chatterbox Turbo
				// TTS pipeline (model bundle, tokenizer, runners, worker).
				// Added so sibling .cpp files under Private/ (smoke tests,
				// future subsystem impl) can #include "InoChatterboxModels.h"
				// and similar without relative paths.
				Path.Combine(ModuleDirectory, "Private", "Chatterbox"),

				// Subdirectory of Private/ that holds the Slate chat-panel
				// implementation (private widgets, style, bridge .cpp).
				// Added so sibling .cpps can #include "InoChatStyle.h"
				// and #include "SInoMessageBubble.h" without relative paths.
				Path.Combine(ModuleDirectory, "Private", "UI", "Slate"),

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

				// Subdirectory of Private/ that holds the NeuTTS Nano
				// on-device TTS subsystem (download orchestration in
				// Milestone 2; runner + worker + voice registry in
				// Milestone 3; synthesis pipeline in Milestone 4).
				// NeuTTS Nano is a pure consumer of the existing llama.cpp
				// vtable (InoLlamaCppModule) and FInoOnnxSession — no
				// dedicated third-party module of its own.
				Path.Combine(ModuleDirectory, "Private", "NeuTtsNano"),
			}
			);


		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",       // UObject, UCLASS, UENUM, dynamic delegate macros — used throughout
				                     // Source/InoAgents/Public/LiteRtLm/ starting at Milestone D.1.
				"Engine",            // UDataAsset, UGameInstanceSubsystem, GEngine, FWorldContext —
				                     // used in ULiteRtLmModelConfig and UInoLiteRtLmSubsystem.
				//"InoAgentsLibrary",  // LiteRT-LM C API via the staged header.
				"InoOnnxRuntime",    // ONNX Runtime (Ort::Session / Env / Value) for generic
				                     // ONNX inference. First consumer: Chatterbox Turbo TTS.
				                     // Set up by Plugins/InoAgents/OnnxRuntime/scripts/setup-onnxruntime.ps1.
				"InoLlamaCpp",       // llama.cpp runtime — GGUF-format LLM inference (Qwen,
				                     // Phi, Llama, SmolLM, DeepSeek-R1-Distill, TinyLlama,
				                     // and future GGUF-based TTS backbones like NeuTTS Nano).
				                     // Set up by Plugins/InoAgents/LlamaCpp/scripts/setup-llamacpp.ps1.
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
				// Slate UI for the in-PIE chat panel (SInoChatPanel and friends).
				// All Slate-using files live under Source/InoAgents/{Public,Private}/UI/Slate/.
				"Slate",
				"SlateCore",

				// Required for EKeys::Enter / EKeys::Escape constants used by the
				// chat input's Enter-to-send and ESC-to-dismiss handling. Forgetting
				// this gives a confusing link error rather than a header error.
				"InputCore",
			}
			);

		// FEditorDelegates::PrePIEEnded is editor-only — used by the chat panel's
		// Show/Hide console commands to tear the panel down BEFORE the GameViewport
		// dies at PIE end. Wrapped in #if WITH_EDITOR at every call site.
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// DXGI system library for IDXGIFactory::EnumAdapters — used by
			// the Ino.Onnx.ListDmlAdapters console command to enumerate D3D12
			// adapters (which is the adapter order DirectML's
			// OrtSessionOptionsAppendExecutionProvider_DML uses for its
			// device_id parameter). System library, part of Windows SDK on
			// every UE build host; no runtime redistribution needed.
			PublicSystemLibraries.Add("dxgi.lib");
		}
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
