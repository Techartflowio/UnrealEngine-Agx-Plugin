// Copyright (c) 2026. TonemapMaster plugin.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FTonemapMasterSceneViewExtension;

class FTonemapMasterModule : public IModuleInterface
{
public:
	//~ IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void OnPostEngineInit();

	TSharedPtr<FTonemapMasterSceneViewExtension, ESPMode::ThreadSafe> SceneViewExtension;
};
