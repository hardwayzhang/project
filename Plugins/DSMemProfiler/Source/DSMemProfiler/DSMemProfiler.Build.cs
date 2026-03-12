// Copyright (c) Project Team. All Rights Reserved.
// DSMemProfiler.Build.cs — only compiles meaningfully on Linux server targets.

using UnrealBuildTool;
using System.IO;

public class DSMemProfiler : ModuleRules
{
	public DSMemProfiler(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(new string[]
		{
			Path.Combine(ModuleDirectory, "Public")
		});

		PrivateIncludePaths.AddRange(new string[]
		{
			Path.Combine(ModuleDirectory, "Private")
		});

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"InputCore",
			"Json",
			"JsonUtilities",
		});

		// Enable only when building Linux Dedicated Server or Editor (for simulation)
		bool bIsLinuxServer = (Target.Platform == UnrealTargetPlatform.Linux) &&
		                      (Target.Type == TargetType.Server || Target.Type == TargetType.Editor);

		if (bIsLinuxServer)
		{
			PublicDefinitions.Add("DS_MEM_PROFILER_ENABLED=1");
		}
		else
		{
			PublicDefinitions.Add("DS_MEM_PROFILER_ENABLED=0");
		}
	}
}
