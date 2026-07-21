// Copyright (c) 2026. TonemapMaster plugin.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateTypes.h"
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"

class IConsoleVariable;
template <typename OptionType> class SComboBox;

/** Artist settings panel driving the TonemapMaster console variables. */
class STonemapMasterPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(STonemapMasterPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	static IConsoleVariable* GetEnableCVar();
	static IConsoleVariable* GetLookCVar();

	ECheckBoxState GetEnableState() const;
	void OnEnableChanged(ECheckBoxState NewState);
	bool IsTonemapperEnabled() const;

	TSharedRef<SWidget> GenerateLookItem(TSharedPtr<FString> Item) const;
	void OnLookSelectionChanged(TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo);
	FText GetCurrentLookText() const;

	TArray<TSharedPtr<FString>> LookOptions;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> LookComboBox;
};
