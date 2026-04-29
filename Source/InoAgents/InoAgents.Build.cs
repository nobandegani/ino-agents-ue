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
				//   - sub-modules (e.g. InoChatterboxOnnx) that depend on
				//     InoAgents inherit the path and can use the same bare
				//     include for cross-module smoke-test reuse.
				Path.Combine(ModuleDirectory, "Public", "SmokeTests"),

				// Subdirectory of Public/ that holds shared audio helpers
				// (InoChatterboxAudioIO.h — mono WAV reader / writer + PCM
				// helpers used by InoAudioFunctionLibrary AND by sub-modules
				// like InoChatterboxOnnx). Exposed so the bare
				// #include "InoChatterboxAudioIO.h" resolves from anywhere.
				Path.Combine(ModuleDirectory, "Public", "Audio"),
			}
			);


		PrivateIncludePaths.AddRange(
			new string[] {
				// Subdirectory of Private/ that holds the phase-1 smoke test
				// .cpp source files. The shared helpers
				// (InoSmokeTestCommon.h) live in Public/SmokeTests/ so
				// sub-modules can reuse them; InoAgentsLog.h is also in
				// Public/ now. This entry exists for any header-only
				// helpers private to the test sources themselves.
				Path.Combine(ModuleDirectory, "Private", "SmokeTests"),

				// Subdirectory of Private/ that holds the LiteRT-LM backend
				// implementation (workers, tool impls, etc.). Added so that
				// InoLiteRtLmSubsystem.cpp can #include "InoLiteRtLmConversationWorker.h"
				// and similar private headers without relative paths.
				Path.Combine(ModuleDirectory, "Private", "LiteRtLm"),

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
				// NeuTTS Nano is a pure consumer of the InoLlama-supplied
				// llama.cpp vtable and FInoOnnxSession — no dedicated
				// third-party module of its own.
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
				"InoLiteRT",         // LiteRT + LiteRT-LM C APIs, supplied by the sibling
				                     // InoLiteRT plugin. Exposes:
				                     //   #include "litert/c/litert_*.h"  (LiteRT TFLite runtime)
				                     //   #include "litert/lm/engine.h"   (LiteRT-LM LLM runtime)
				                     // and stages the runtime DLLs/.so for packaging. The plugin
				                     // pre-loads its DLLs at LoadingPhase=PreLoadingScreen so they
				                     // are callable by the time this module's StartupModule runs.
				"InoOnnx",           // ONNX Runtime (Ort::Session / Env / Value) supplied by the
				                     // sibling InoOnnx plugin. Exposes:
				                     //   #include "onnxruntime_c_api.h"   (ORT C API)
				                     //   #include "InoOnnx.h"             (InoOnnx::GetApi() accessor)
				                     // and stages the runtime DLLs/.so for packaging. The plugin
				                     // pre-loads its DLLs at LoadingPhase=PreLoadingScreen so they
				                     // are callable by the time this module's StartupModule runs.
				                     // First consumer: Chatterbox Turbo TTS.
				"InoLlama",          // llama.cpp runtime — supplied by the sibling InoLlama
				                     // plugin. Exposes:
				                     //   #include "llama.h"               (llama.cpp C API)
				                     //   #include "InoLlama.h"            (InoAgents::LlamaCpp::GetApi() accessor + FLlamaCppApi vtable)
				                     // and stages the runtime DLLs/.so for packaging. The plugin
				                     // pre-loads its DLLs at LoadingPhase=PreLoadingScreen so they
				                     // are callable by the time this module's StartupModule runs.
				                     // First consumer: NeuTTS Nano TTS.
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
