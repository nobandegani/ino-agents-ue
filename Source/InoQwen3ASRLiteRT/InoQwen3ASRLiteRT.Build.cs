// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

/// <summary>
/// Qwen3-ASR-0.6B speech-to-text via LiteRT.
///
/// Sub-module of InoAgents that wraps the litert-community-exported
/// qwen3_asr_0.6b_5s_i8.tflite (~794 MB, int8-quantized 5-second window,
/// encoder-decoder transformer with autoregressive decoding) and exposes
/// it through a UE subsystem with an async TranscribeAsync API.
///
/// Self-contained / deletable: removing this directory + the Modules entry
/// in InoAgents.uplugin removes the entire module without affecting other
/// InoAgents sub-modules.
///
/// Source model: https://huggingface.co/Qwen/Qwen3-ASR-0.6B
/// LiteRT export:  https://huggingface.co/litert-community/Qwen3-ASR-0.6B
/// </summary>
public class InoQwen3ASRLiteRT : ModuleRules
{
    public InoQwen3ASRLiteRT(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // Disable unity build — anonymous-namespace constants in the smoke
        // tests (kSampleRate, kAudioWindowSamples, etc.) would clash if
        // merged into one unity TU.
        bUseUnity = false;

        PrivateIncludePaths.AddRange(
            new string[] {
                Path.Combine(ModuleDirectory, "Private", "Qwen3ASR"),
                Path.Combine(ModuleDirectory, "Private", "SmokeTests"),
            }
            );

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",

                "InoAgents",       // Shared logging / audio helpers
                                   // (UInoAudioFunctionLibrary for WAV I/O
                                   // when Phase 4 lands).

                "InoLiteRT",       // The LiteRT C API (libLiteRt.dll on Win64,
                                   // monolithic libLiteRtLm.so on Android).
            }
            );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "Json",            // Tokenizer.json parsing in Phase 3.
                "JsonUtilities",

                "Projects",        // IPluginManager::Get for resolving the
                                   // model path inside the plugin's BaseDir.
            }
            );
    }
}
