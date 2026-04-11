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
				                     // (used by Phase 1 smoke tests and by D.4 tool-call parsing).
				"Projects",          // IPluginManager for locating the plugin's base directory at runtime.
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// ... add private dependencies that you statically link with here ...	
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
