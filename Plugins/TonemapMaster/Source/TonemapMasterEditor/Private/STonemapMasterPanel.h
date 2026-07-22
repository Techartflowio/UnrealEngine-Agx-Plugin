// Copyright (c) 2026. TonemapMaster plugin.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateTypes.h"
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"

class IConsoleVariable;
template <typename OptionType> class SComboBox;

/** Artist settings panel driving the TonemapMaster console variables (Unified Tonemapper System). */
class STonemapMasterPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(STonemapMasterPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	static IConsoleVariable* GetEnableCVar();
	static IConsoleVariable* GetModeCVar();
	static IConsoleVariable* GetLookCVar();

	ECheckBoxState GetEnableState() const;
	void OnEnableChanged(ECheckBoxState NewState);
	bool IsTonemapperEnabled() const;

	// Tonemapper mode (r.TonemapMaster.Mode: 0 = AgX, 1 = GT7)
	TSharedRef<SWidget> GenerateModeItem(TSharedPtr<FString> Item) const;
	void OnModeSelectionChanged(TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo);
	FText GetCurrentModeText() const;
	int32 GetCurrentMode() const;
	bool IsAgXModeSelected() const;
	bool IsGT7ModeSelected() const;

	// Look selection (r.TonemapMaster.Look) — AgX mode only
	TSharedRef<SWidget> GenerateLookItem(TSharedPtr<FString> Item) const;
	void OnLookSelectionChanged(TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo);
	FText GetCurrentLookText() const;
	bool IsLookEnabled() const;

	// GT7 float parameters (r.TonemapMaster.GT7.*)
	float GetGT7CVarValue(const TCHAR* Name) const;
	void SetGT7CVarValue(const TCHAR* Name, float Value);
	bool IsGT7ControlsEnabled() const;
	TSharedRef<SWidget> MakeGT7Row(const FText& Label, const FText& ToolTip, const TCHAR* CVarName, float MinValue, float MaxValue, float Delta);

	TArray<TSharedPtr<FString>> ModeOptions;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> ModeComboBox;

	TArray<TSharedPtr<FString>> LookOptions;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> LookComboBox;
};
