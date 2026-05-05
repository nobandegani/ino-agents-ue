// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using UnrealBuildTool;

/// <summary>
/// Editor-only support for the NeuTTS sub-module:
///   - UInoNeuTtsVoiceFactory: import factory for `.inv` voice files
///     (drag/drop into Content Browser; re-import on source change).
///
/// Type=Editor in the .uplugin so this module is excluded from cooked
/// game / shipping targets — none of UFactory / FReimportHandler / the
/// AssetTools registration ships at runtime.
/// </summary>
public class InoNeuTtsNativeEditor : ModuleRules
{
	public InoNeuTtsNativeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",

			// Runtime sister module — owns UInoNeuTtsVoiceAsset that
			// the factory creates instances of, plus FInoNeuTtsVoice
			// for the JSON shape we parse.
			"InoNeuTtsNative",
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
		});
	}
}
