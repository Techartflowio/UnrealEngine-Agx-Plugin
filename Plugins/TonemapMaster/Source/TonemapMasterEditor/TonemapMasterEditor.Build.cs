// Copyright (c) 2026. TonemapMaster plugin.

using UnrealBuildTool;

public class TonemapMasterEditor : ModuleRules
{
	public TonemapMasterEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"InputCore",
				"Slate",
				"SlateCore",
				"ToolMenus",
			}
		);
	}
}
