// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using UnrealBuildTool;

/// <summary>
/// Editor-only support for the InoNeuTTS sub-module:
///   - UInoNeuTTSVoiceFactory: import factory for `.inv` voice files
///     (drag/drop into Content Browser; re-import on source change).
///
/// Type=Editor in the .uplugin so this module is excluded from cooked
/// game / shipping targets — none of UFactory / FReimportHandler / the
/// AssetTools registration ships at runtime.
///
/// Self-contained deletion unit: rm this folder + drop the entry from
/// InoAgents.uplugin's Modules array and the entire .inv import
/// workflow goes with it. The runtime InoNeuTTS module stays buildable
/// because UInoNeuTTSVoiceAsset's editor-only AssetImportData property
/// is guarded by WITH_EDITORONLY_DATA.
/// </summary>
public class InoNeuTTSEditor : ModuleRules
{
    public InoNeuTTSEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",

            // Runtime sister module — owns UInoNeuTTSVoiceAsset that
            // the factory creates instances of.
            "InoNeuTTS",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "UnrealEd",            // UFactory / FReimportHandler base classes
            "AssetTools",          // (registration not strictly required —
                                   //  Formats list on the factory is enough —
                                   //  but link in case we want to register
                                   //  asset categories later)
            "Json",                // FJsonObject / FJsonSerializer
            "JsonUtilities",
            "EditorFramework",     // UAssetImportData
            "InoAgents",           // shared LogInoAgents category
        });
    }
}
