// Copyright CamSim Contributors. All Rights Reserved.

using UnrealBuildTool;

public class CamSimShaders : ModuleRules
{
	public CamSimShaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "Engine", "RenderCore", "RHI", "Renderer" });
	}
}
