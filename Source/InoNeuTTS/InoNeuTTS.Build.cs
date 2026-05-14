// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

/// <summary>
/// Neuphonic NeuTTS Nano on-device TTS — sub-module of the InoAgents
/// plugin, sibling to InoLiteRtLm.
///
/// Backbone (the speech-token LLM) is consumed as a .litertlm bundle
/// through the LiteRT-LM C API (`litert/lm/engine.h`). Decoder
/// (NeuCodec, the speech-token-to-waveform model) is consumed as a
/// .tflite through the bare LiteRT C API (`litert/c/litert_*.h`).
/// Both runtime DLLs/.so are pre-loaded by the sibling InoLiteRT
/// plugin at LoadingPhase=PreLoadingScreen, so by the time this
/// Default-phase StartupModule runs both C APIs are callable.
///
/// Self-contained deletion unit (mirrors InoLiteRtLm): rm this folder
/// + drop the entry from InoAgents.uplugin's Modules array and the
/// entire NeuTTS impl goes with it (subsystem, settings page, voice
/// asset, smoke tests). InoAgents.Build.cs deliberately does NOT
/// depend on this module — game code reaches the NeuTTS subsystem via
/// UGameInstance::GetSubsystem on demand, which keeps the
/// deprecation-as-folder-delete property intact.
/// </summary>
public class InoNeuTTS : ModuleRules
{
    public InoNeuTTS(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        // Disable unity build: anonymous-namespace constants in smoke
        // tests (Ino.NeuTTS.* commands) collide when merged into a
        // single unity TU. Same reason as InoLiteRtLm.
        bUseUnity = false;

        PrivateIncludePaths.AddRange(
            new string[] {
                // Holds NeuTTS-internal headers — runner, prompt builder,
                // engine backend wrapper, decoder session wrapper, etc.
                // Listed so private .cpps can include each other's
                // headers without relative paths.
                Path.Combine(ModuleDirectory, "Private", "NeuTTS"),

                // Smoke tests live alongside, including the Phase 0
                // spikes (DecoderProbeTest, BackboneSpikeTest).
                Path.Combine(ModuleDirectory, "Private", "SmokeTests"),
            }
            );

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",       // UCLASS / USTRUCT / UENUM macros — used in
                                     // Public/NeuTTS/ for FInoNeuTTSConfig etc.
                "Engine",            // UGameInstanceSubsystem — UInoNeuTTSSubsystem.

                "InoAgents",         // Shared LogInoAgents category +
                                     // UInoAudioFunctionLibrary (mono PCM
                                     // helpers: SaveInt16PcmAsWav,
                                     // Float32ToInt16PcmBytesMono,
                                     // GenerateDitheredSilence) +
                                     // InoSmokeTestCommon helper. The NeuTTS
                                     // subsystem reads its OWN settings, so no
                                     // dep on UInoAgentsSettings is needed.

                "InoLiteRT",         // LiteRT + LiteRT-LM C APIs — the entire
                                     // reason this module exists. Provides:
                                     //   #include "litert/c/litert_*.h"   (bare TFLite — NeuCodec decoder)
                                     //   #include "litert/lm/engine.h"    (LLM runtime — NeuTTS backbone)
                                     // and stages the runtime DLLs/.so for
                                     // packaging at PreLoadingScreen.

                "InoNodes",          // Generic file downloader + SHA-256
                                     // helpers (InoNodes::Download::DownloadFileAsync,
                                     // FInoDownloadProgress, FInoCancellationToken).
                                     // Public dep so FInoDownloadProgress is
                                     // reachable through the
                                     // FInoNeuTTSDownloadProgressDelegate
                                     // declared in InoNeuTTSTypes.h.

                "InoSpeakNG",        // eSpeak NG phonemization for the input
                                     // text + reference-voice transcript.
                                     // NeuTTS is trained on IPA phonemes;
                                     // espeak-ng is the matching phonemizer.
                                     // GPLv3 dynamic-linkage carve-out.

                "Json",              // FJsonObject / FJsonSerializer — used
                                     // by any future .inv voice-asset parsing
                                     // (deferred — voices come later).
                "JsonUtilities",     // FJsonObjectWrapper.
                "JsonBlueprintUtilities", // GetField/SetField/HasField BP nodes.

                "DeveloperSettings", // UDeveloperSettings base class for
                                     // UInoNeuTTSSettings.

                "RuntimeAudioImporter", // UStreamingSoundWave +
                                     // ERuntimeRAWAudioFormat — the streaming
                                     // audio sink consumers typically feed
                                     // the synthesized 24 kHz mono int16 PCM
                                     // into.
            }
            );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "Projects",          // IPluginManager — for plugin base-dir
                                     // lookup (legacy dev-path resource
                                     // fallback for any bundled assets).
            }
            );

        // EditorFramework supplies UAssetImportData, referenced by the
        // editor-only UPROPERTY on UInoNeuTTSVoiceAsset (used by the
        // .inv UFactory to track the source file for Reimport).
        // Guarded so cooked / shipping builds don't pull editor deps.
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.Add("EditorFramework");
        }
    }
}
