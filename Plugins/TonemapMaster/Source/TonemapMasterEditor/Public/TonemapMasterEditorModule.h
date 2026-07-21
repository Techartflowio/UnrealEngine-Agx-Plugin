// Copyright (c) 2026. TonemapMaster plugin.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FSpawnTabArgs;
class SDockTab;
class UToolMenu;

/**
 * Editor-only module: adds the "Mazeline" main-menu entry (right of the Actor
 * menu) with a "Tonemap Master" item that opens the artist settings panel.
 */
class FTonemapMasterEditorModule : public IModuleInterface
{
public:
	//~ IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	void FillMazelineMenu(UToolMenu* Menu);
	TSharedRef<SDockTab> SpawnTonemapMasterTab(const FSpawnTabArgs& Args);
};
