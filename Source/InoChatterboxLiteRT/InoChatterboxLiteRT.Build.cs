// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

/// <summary>
/// Chatterbox Turbo TTS via LiteRT (Google's TFLite-based runtime) — sub-module
/// of the InoAgents plugin. Sibling of InoChatterboxNative (which uses ONNX),
/// but consumes ONLY LiteRT — no onnxruntime dependency.
///
/// Self-contained: deletes cleanly when the implementation is deprecated by
/// removing this directory + its entry in InoAgents.uplugin's Modules array.
///
/// The Build.cs sibling InoAgents.Build.cs deliberately does NOT depend on
/// InoChatterboxLiteRT — sub-modules of the plugin are reached via game code
/// that loads the relevant subsystem on demand. Adding a reverse dep here
/// would defeat the deprecation-as-folder-delete property.
/// </summary>
public class InoChatterboxLiteRT : ModuleRules
{
    public InoChatterboxLiteRT(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // Disable unity build for this module: anonymous-namespace constants
        // (kStartSpeechToken, kHiddenSize, etc.) shared across runner/sampler/
        // tokenizer translation units would clash if merged into one unity TU.
        // Same rationale as InoChatterboxNative.Build.cs.
        bUseUnity = false;

        PublicIncludePaths.AddRange(
            new string[] {
            }
            );

        PrivateIncludePaths.AddRange(
            new string[] {
                // Subdirectory of Private/ that holds the runner + workers +
                // tokenizer + voice loader. The .cpps include each other with
                // bare names (e.g. #include "InoChatterboxLiteRTRunner.h").
                Path.Combine(ModuleDirectory, "Private", "ChatterboxLiteRT"),

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
                                    // Public/ for FInoChatterboxLiteRTVoice etc.
                "Engine",           // UGameInstanceSubsystem — UInoChatterboxLiteRTSubsystem.

                "InoAgents",        // For the shared LogInoAgents category +
                                    // InoSmokeTestCommon helper. The subsystem
                                    // reads its OWN settings, so no dep on
                                    // InoAgents Settings is needed.

                "InoLiteRT",        // LiteRT C API — the entire reason this
                                    // module exists. Provides the LiteRt* C API
                                    // headers and links libLiteRt.{lib,dll}.

                "DeveloperSettings", // UDeveloperSettings base class for
                                     // UInoChatterboxLiteRTSettings.

                "RuntimeAudioImporter", // UStreamingSoundWave +
                                        // ERuntimeAudioFormat / ERuntimeRAWAudioFormat —
                                        // the streaming audio sink (Phase 4+).

                "HTTP",             // FHttpModule for the multi-file model
                                    // download flow (Phase 6).
            }
            );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "Json",          // FJsonObject / FJsonSerializer for parsing
                                 // tokenizer.json (Phase 2).
                "JsonUtilities",

                "Projects",      // IPluginManager::Get — used by smoke tests
                                 // and runner to resolve InoAgents plugin's
                                 // BaseDir for staged Chatterbox model files.
            }
            );

        DynamicallyLoadedModuleNames.AddRange(
            new string[]
            {
            }
            );
    }
}
