// Copyright (c) 2026. TonemapMaster plugin.

#include "TonemapMasterModule.h"

#include "TonemapMasterSceneViewExtension.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "SceneViewExtension.h"
#include "ShaderCore.h"

void FTonemapMasterModule::StartupModule()
{
	// Map the plugin's virtual shader directory before any global shader is compiled.
	// This is why the module uses the PostConfigInit loading phase.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("TonemapMaster"));
	if (Plugin.IsValid())
	{
		const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/TonemapMaster"), ShaderDir);
	}

	// The scene view extension registry is only usable once the engine has initialized.
	FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FTonemapMasterModule::OnPostEngineInit);
}

void FTonemapMasterModule::OnPostEngineInit()
{
	if (FApp::CanEverRender())
	{
		SceneViewExtension = FSceneViewExtensions::NewExtension<FTonemapMasterSceneViewExtension>();
	}
}

void FTonemapMasterModule::ShutdownModule()
{
	FCoreDelegates::GetOnPostEngineInit().RemoveAll(this);
	SceneViewExtension.Reset();
}

IMPLEMENT_MODULE(FTonemapMasterModule, TonemapMaster)
