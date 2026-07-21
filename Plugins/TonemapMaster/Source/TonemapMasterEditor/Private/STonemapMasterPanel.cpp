// Copyright (c) 2026. TonemapMaster plugin.

#include "STonemapMasterPanel.h"

#include "HAL/IConsoleManager.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "TonemapMasterEditor"

namespace TonemapMasterPanel
{
	// Combo indices map directly to r.TonemapMaster.Look values.
	static const TCHAR* LookNames[] = { TEXT("Default"), TEXT("Golden"), TEXT("Punchy") };
}

IConsoleVariable* STonemapMasterPanel::GetEnableCVar()
{
	return IConsoleManager::Get().FindConsoleVariable(TEXT("r.TonemapMaster.Enable"));
}

IConsoleVariable* STonemapMasterPanel::GetLookCVar()
{
	return IConsoleManager::Get().FindConsoleVariable(TEXT("r.TonemapMaster.Look"));
}

void STonemapMasterPanel::Construct(const FArguments& InArgs)
{
	for (const TCHAR* LookName : TonemapMasterPanel::LookNames)
	{
		LookOptions.Add(MakeShared<FString>(LookName));
	}

	const float RowPadding = 6.0f;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)

			// Header
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, RowPadding)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PanelHeader", "Tonemap Master — AgX"))
				.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, RowPadding)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PanelDescription", "Replaces the engine filmic tonemapper with the AgX tonemapper.\nChanges apply immediately to all viewports."))
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, RowPadding)
			[
				SNew(SSeparator)
			]

			// Enable toggle (r.TonemapMaster.Enable)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, RowPadding)
			[
				SNew(SCheckBox)
				.IsChecked(this, &STonemapMasterPanel::GetEnableState)
				.OnCheckStateChanged(this, &STonemapMasterPanel::OnEnableChanged)
				.ToolTipText(LOCTEXT("EnableToolTip", "r.TonemapMaster.Enable — off falls back to the engine filmic tonemapper."))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("EnableLabel", "Enable AgX Tonemapper"))
				]
			]

			// Look selection (r.TonemapMaster.Look)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, RowPadding)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LookLabel", "Look"))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SAssignNew(LookComboBox, SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&LookOptions)
					.OnGenerateWidget(this, &STonemapMasterPanel::GenerateLookItem)
					.OnSelectionChanged(this, &STonemapMasterPanel::OnLookSelectionChanged)
					.IsEnabled(this, &STonemapMasterPanel::IsTonemapperEnabled)
					.ToolTipText(LOCTEXT("LookToolTip", "r.TonemapMaster.Look — AgX look applied after the sigmoid (Default / Golden / Punchy)."))
					[
						SNew(STextBlock)
						.Text(this, &STonemapMasterPanel::GetCurrentLookText)
					]
				]
			]
		]
	];
}

ECheckBoxState STonemapMasterPanel::GetEnableState() const
{
	const IConsoleVariable* CVar = GetEnableCVar();
	return (CVar && CVar->GetInt() != 0) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void STonemapMasterPanel::OnEnableChanged(ECheckBoxState NewState)
{
	if (IConsoleVariable* CVar = GetEnableCVar())
	{
		CVar->Set(NewState == ECheckBoxState::Checked ? 1 : 0, ECVF_SetByConsole);
	}
}

bool STonemapMasterPanel::IsTonemapperEnabled() const
{
	return GetEnableState() == ECheckBoxState::Checked;
}

TSharedRef<SWidget> STonemapMasterPanel::GenerateLookItem(TSharedPtr<FString> Item) const
{
	return SNew(STextBlock).Text(FText::FromString(*Item));
}

void STonemapMasterPanel::OnLookSelectionChanged(TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo)
{
	if (!Item.IsValid())
	{
		return;
	}

	if (IConsoleVariable* CVar = GetLookCVar())
	{
		const int32 LookIndex = LookOptions.IndexOfByKey(Item);
		if (LookIndex != INDEX_NONE)
		{
			CVar->Set(LookIndex, ECVF_SetByConsole);
		}
	}
}

FText STonemapMasterPanel::GetCurrentLookText() const
{
	const IConsoleVariable* CVar = GetLookCVar();
	const int32 LookIndex = CVar ? FMath::Clamp(CVar->GetInt(), 0, (int32)UE_ARRAY_COUNT(TonemapMasterPanel::LookNames) - 1) : 0;
	return FText::FromString(TonemapMasterPanel::LookNames[LookIndex]);
}

#undef LOCTEXT_NAMESPACE
