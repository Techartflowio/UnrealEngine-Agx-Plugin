// Copyright (c) 2026. TonemapMaster plugin.

#include "STonemapMasterPanel.h"

#include "HAL/IConsoleManager.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "TonemapMasterEditor"

namespace TonemapMasterPanel
{
	// Combo indices map directly to r.TonemapMaster.Mode values (0 = AgX, 1 = GT7).
	static const TCHAR* ModeNames[] = { TEXT("AgX"), TEXT("GT7 (Color Volume Mapping)") };

	// Combo indices map directly to r.TonemapMaster.Look values.
	static const TCHAR* LookNames[] = { TEXT("Default"), TEXT("Golden"), TEXT("Punchy") };

	struct FGT7Param
	{
		const TCHAR* CVarName;
		float MinValue;
		float MaxValue;
		float Delta;
	};

	// Ranges mirror the ECVF ranges declared on the runtime CVars.
	static const FGT7Param GT7Params[] =
	{
		{ TEXT("r.TonemapMaster.GT7.TargetLuminance"),    250.0f, 10000.0f, 10.0f  },
		{ TEXT("r.TonemapMaster.GT7.BlendRatio"),           0.0f,     1.0f,  0.01f },
		{ TEXT("r.TonemapMaster.GT7.CurveMidPoint"),        0.1f,     1.0f,  0.001f },
		{ TEXT("r.TonemapMaster.GT7.CurveLinearSection"),   0.1f,     2.0f,  0.001f },
		{ TEXT("r.TonemapMaster.GT7.CurveToeStrength"),     0.5f,     3.0f,  0.01f },
		{ TEXT("r.TonemapMaster.GT7.CurveAlpha"),          0.01f,     1.5f,  0.001f },
		{ TEXT("r.TonemapMaster.GT7.ChromaFadeStart"),      0.0f,     2.0f,  0.01f },
		{ TEXT("r.TonemapMaster.GT7.ChromaFadeEnd"),        0.0f,     2.0f,  0.01f },
	};
}

IConsoleVariable* STonemapMasterPanel::GetEnableCVar()
{
	return IConsoleManager::Get().FindConsoleVariable(TEXT("r.TonemapMaster.Enable"));
}

IConsoleVariable* STonemapMasterPanel::GetModeCVar()
{
	return IConsoleManager::Get().FindConsoleVariable(TEXT("r.TonemapMaster.Mode"));
}

IConsoleVariable* STonemapMasterPanel::GetLookCVar()
{
	return IConsoleManager::Get().FindConsoleVariable(TEXT("r.TonemapMaster.Look"));
}

void STonemapMasterPanel::Construct(const FArguments& InArgs)
{
	for (const TCHAR* ModeName : TonemapMasterPanel::ModeNames)
	{
		ModeOptions.Add(MakeShared<FString>(ModeName));
	}

	for (const TCHAR* LookName : TonemapMasterPanel::LookNames)
	{
		LookOptions.Add(MakeShared<FString>(LookName));
	}

	// Human-facing labels/tooltips for each GT7 row, parallel to TonemapMasterPanel::GT7Params.
	const FText GT7Labels[] =
	{
		LOCTEXT("GT7TargetLuminanceLabel", "Target Luminance (nits)"),
		LOCTEXT("GT7BlendRatioLabel", "Color Preserve Blend"),
		LOCTEXT("GT7CurveMidPointLabel", "Curve Mid Point"),
		LOCTEXT("GT7CurveLinearSectionLabel", "Curve Linear Section"),
		LOCTEXT("GT7CurveToeStrengthLabel", "Curve Toe Strength"),
		LOCTEXT("GT7CurveAlphaLabel", "Curve Alpha"),
		LOCTEXT("GT7ChromaFadeStartLabel", "Chroma Fade Start"),
		LOCTEXT("GT7ChromaFadeEndLabel", "Chroma Fade End"),
	};
	const FText GT7ToolTips[] =
	{
		LOCTEXT("GT7TargetLuminanceToolTip", "r.TonemapMaster.GT7.TargetLuminance — HDR peak luminance target. No effect on SDR output."),
		LOCTEXT("GT7BlendRatioToolTip", "r.TonemapMaster.GT7.BlendRatio — 0 = per-channel curve, 1 = full ICtCp chroma preservation."),
		LOCTEXT("GT7CurveMidPointToolTip", "r.TonemapMaster.GT7.CurveMidPoint — toe/linear transition."),
		LOCTEXT("GT7CurveLinearSectionToolTip", "r.TonemapMaster.GT7.CurveLinearSection — linear/shoulder boundary (fraction of peak)."),
		LOCTEXT("GT7CurveToeStrengthToolTip", "r.TonemapMaster.GT7.CurveToeStrength — toe gamma; >1 darkens shadows/midtones."),
		LOCTEXT("GT7CurveAlphaToolTip", "r.TonemapMaster.GT7.CurveAlpha — shoulder convergence; lower = harder clip."),
		LOCTEXT("GT7ChromaFadeStartToolTip", "r.TonemapMaster.GT7.ChromaFadeStart — chroma fade-in of desaturation near peak (relative to target I)."),
		LOCTEXT("GT7ChromaFadeEndToolTip", "r.TonemapMaster.GT7.ChromaFadeEnd — chroma fully desaturated beyond this point (relative to target I)."),
	};
	static_assert(UE_ARRAY_COUNT(GT7Labels) == UE_ARRAY_COUNT(TonemapMasterPanel::GT7Params), "GT7 label count must match param count");
	static_assert(UE_ARRAY_COUNT(GT7ToolTips) == UE_ARRAY_COUNT(TonemapMasterPanel::GT7Params), "GT7 tooltip count must match param count");

	const float RowPadding = 6.0f;

	TSharedRef<SVerticalBox> MainBox = SNew(SVerticalBox)

		// Header
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, RowPadding)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PanelHeader", "Tonemap Master — Unified Tonemapper System"))
			.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, RowPadding)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PanelDescription", "Replaces the engine filmic tonemapper with AgX or GT7 Color Volume Mapping.\nChanges apply immediately to all viewports."))
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
				.Text(LOCTEXT("EnableLabel", "Enable Tonemap Master"))
			]
		]

		// Tonemapper selection (r.TonemapMaster.Mode)
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
				.Text(LOCTEXT("ModeLabel", "Tonemapper"))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SAssignNew(ModeComboBox, SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&ModeOptions)
				.OnGenerateWidget(this, &STonemapMasterPanel::GenerateModeItem)
				.OnSelectionChanged(this, &STonemapMasterPanel::OnModeSelectionChanged)
				.IsEnabled(this, &STonemapMasterPanel::IsTonemapperEnabled)
				.ToolTipText(LOCTEXT("ModeToolTip", "r.TonemapMaster.Mode — 0 = AgX, 1 = GT7 Color Volume Mapping."))
				[
					SNew(STextBlock)
					.Text(this, &STonemapMasterPanel::GetCurrentModeText)
				]
			]
		]

		// Look selection (r.TonemapMaster.Look) — AgX mode only
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
				.IsEnabled(this, &STonemapMasterPanel::IsLookEnabled)
				.ToolTipText(LOCTEXT("LookToolTip", "r.TonemapMaster.Look — AgX look applied after the sigmoid (Default / Golden / Punchy). AgX mode only."))
				[
					SNew(STextBlock)
					.Text(this, &STonemapMasterPanel::GetCurrentLookText)
				]
			]
		]

		// GT7 section header
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, RowPadding, 0.0f, RowPadding)
		[
			SNew(SSeparator)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, RowPadding)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("GT7SectionHeader", "GT7 Color Volume Mapping"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];

	// One spin box row per GT7 float parameter.
	for (int32 ParamIndex = 0; ParamIndex < (int32)UE_ARRAY_COUNT(TonemapMasterPanel::GT7Params); ++ParamIndex)
	{
		const TonemapMasterPanel::FGT7Param& Param = TonemapMasterPanel::GT7Params[ParamIndex];

		MainBox->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, RowPadding)
		[
			MakeGT7Row(GT7Labels[ParamIndex], GT7ToolTips[ParamIndex], Param.CVarName, Param.MinValue, Param.MaxValue, Param.Delta)
		];
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			MainBox
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

TSharedRef<SWidget> STonemapMasterPanel::GenerateModeItem(TSharedPtr<FString> Item) const
{
	return SNew(STextBlock).Text(FText::FromString(*Item));
}

void STonemapMasterPanel::OnModeSelectionChanged(TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo)
{
	if (!Item.IsValid())
	{
		return;
	}

	if (IConsoleVariable* CVar = GetModeCVar())
	{
		const int32 ModeIndex = ModeOptions.IndexOfByKey(Item);
		if (ModeIndex != INDEX_NONE)
		{
			CVar->Set(ModeIndex, ECVF_SetByConsole);
		}
	}
}

FText STonemapMasterPanel::GetCurrentModeText() const
{
	return FText::FromString(TonemapMasterPanel::ModeNames[GetCurrentMode()]);
}

int32 STonemapMasterPanel::GetCurrentMode() const
{
	const IConsoleVariable* CVar = GetModeCVar();
	return CVar ? FMath::Clamp(CVar->GetInt(), 0, (int32)UE_ARRAY_COUNT(TonemapMasterPanel::ModeNames) - 1) : 0;
}

bool STonemapMasterPanel::IsAgXModeSelected() const
{
	return GetCurrentMode() == 0;
}

bool STonemapMasterPanel::IsGT7ModeSelected() const
{
	return GetCurrentMode() == 1;
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

bool STonemapMasterPanel::IsLookEnabled() const
{
	// The Look is applied by the AgX path only; it has no effect in GT7 mode.
	return IsTonemapperEnabled() && IsAgXModeSelected();
}

float STonemapMasterPanel::GetGT7CVarValue(const TCHAR* Name) const
{
	const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name);
	return CVar ? CVar->GetFloat() : 0.0f;
}

void STonemapMasterPanel::SetGT7CVarValue(const TCHAR* Name, float Value)
{
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
	{
		CVar->Set(Value, ECVF_SetByConsole);
	}
}

bool STonemapMasterPanel::IsGT7ControlsEnabled() const
{
	return IsTonemapperEnabled() && IsGT7ModeSelected();
}

TSharedRef<SWidget> STonemapMasterPanel::MakeGT7Row(const FText& Label, const FText& ToolTip, const TCHAR* CVarName, float MinValue, float MaxValue, float Delta)
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(Label)
			.ToolTipText(ToolTip)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SSpinBox<float>)
			.MinValue(MinValue)
			.MaxValue(MaxValue)
			.MinSliderValue(MinValue)
			.MaxSliderValue(MaxValue)
			.Delta(Delta)
			.MinDesiredWidth(90.0f)
			.Value_Lambda([this, CVarName]() -> float
			{
				return GetGT7CVarValue(CVarName);
			})
			.OnValueChanged_Lambda([this, CVarName](float NewValue)
			{
				SetGT7CVarValue(CVarName, NewValue);
			})
			.IsEnabled(this, &STonemapMasterPanel::IsGT7ControlsEnabled)
			.ToolTipText(ToolTip)
		];
}

#undef LOCTEXT_NAMESPACE
