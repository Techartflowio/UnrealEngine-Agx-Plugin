// Copyright (c) 2026. TonemapMaster plugin.

#include "TonemapMasterEditorModule.h"

#include "STonemapMasterPanel.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "TonemapMasterEditor"

static const FName TonemapMasterTabName("TonemapMasterPanel");

void FTonemapMasterEditorModule::StartupModule()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		TonemapMasterTabName,
		FOnSpawnTab::CreateRaw(this, &FTonemapMasterEditorModule::SpawnTonemapMasterTab))
		.SetDisplayName(LOCTEXT("TabDisplayName", "Tonemap Master"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Artist settings for the TonemapMaster unified tonemapper (AgX / GT7)."))
		.SetMenuType(ETabSpawnerMenuType::Hidden)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "ColorSpace.Icon"));

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FTonemapMasterEditorModule::RegisterMenus));
}

void FTonemapMasterEditorModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TonemapMasterTabName);
	}
}

void FTonemapMasterEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* MainMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu");
	if (!MainMenu)
	{
		return;
	}

	FToolMenuSection& Section = MainMenu->FindOrAddSection(NAME_None);

	FToolMenuEntry& MazelineEntry = Section.AddSubMenu(
		"Mazeline",
		LOCTEXT("MazelineMenu", "Mazeline"),
		LOCTEXT("MazelineMenu_ToolTip", "Mazeline tools"),
		FNewToolMenuDelegate::CreateRaw(this, &FTonemapMasterEditorModule::FillMazelineMenu));

	// The level editor menu bar entry that shows as "Actor" is named "Actions"
	// (its label follows the current selection); insert right of it.
	MazelineEntry.InsertPosition = FToolMenuInsert("Actions", EToolMenuInsertType::After);
}

void FTonemapMasterEditorModule::FillMazelineMenu(UToolMenu* Menu)
{
	FToolMenuSection& Section = Menu->AddSection("MazelineTools", LOCTEXT("MazelineToolsSection", "Rendering"));

	Section.AddMenuEntry(
		"TonemapMaster",
		LOCTEXT("TonemapMasterEntry", "Tonemap Master"),
		LOCTEXT("TonemapMasterEntry_ToolTip", "Open the Tonemap Master artist settings panel."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ColorSpace.Icon"),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(TonemapMasterTabName);
		})));
}

TSharedRef<SDockTab> FTonemapMasterEditorModule::SpawnTonemapMasterTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(STonemapMasterPanel)
		];
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FTonemapMasterEditorModule, TonemapMasterEditor)
