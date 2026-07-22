// Copyright (c) 2026. TonemapMaster plugin.
//
// Pass A — AgX grading-LUT builder. Port of the engine's
//   Source/Runtime/Renderer/Private/PostProcess/PostProcessCombineLUTs.cpp (UE 5.8)
// adapted to FSceneView-only inputs. Sections are annotated with the engine
// lines they were ported from.
//
// LUT CACHING DECISION: the engine caches on FSceneViewState (persistent
// CombinedLUTRenderTarget + FCachedLUTSettings with field-by-field change
// detection). FSceneViewState is Renderer-private and unreachable from a
// plugin, so instead we keep ONE persistent RHI volume texture on the scene
// view extension and rebuild it only when a CityHash64 of every input that
// affects the LUT contents changes (settings, CVars, LUT blend textures,
// output device params, AgX look/contrast mode, working color space, shader
// platform). Unlike the engine's cache the hash deliberately excludes the
// view key: the LUT contents do not depend on the view, only on settings, so
// multiple views with identical post-process settings share the LUT without
// rebuilds. The key also covers the tonemapper mode (AgX vs GT7) and all GT7
// Color Volume Mapping settings, so switching or tuning the tonemapper forces
// a rebuild. The flip side is that views with DIFFERENT settings thrash the
// single slot (rebuild per view per frame); a 32^3 compute dispatch is cheap
// and multi-viewport editing with mismatched grading is rare, so this is an
// acceptable trade-off. r.LUT.UpdateEveryFrame is not ported; set
// r.TonemapMaster.Enable=0 to fall back to the engine path entirely.

#include "TonemapMasterCombineLUTs.h"

#include "Hash/CityHash.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/Scene.h"
#include "Engine/Texture.h"
#include "FinalPostProcessSettings.h"
#include "GlobalRenderResources.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "PixelFormat.h"
#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "TextureResource.h"
#include "UnrealClient.h"

namespace
{

// Engine CVars live in Renderer-private TUs; look them up by name instead of
// redeclaring them (redeclaration would assert on duplicate registration).
const TConsoleVariableData<float>* FindCVarFloat(const TCHAR* Name)
{
	return IConsoleManager::Get().FindTConsoleVariableDataFloat(Name);
}

const TConsoleVariableData<int32>* FindCVarInt(const TCHAR* Name)
{
	return IConsoleManager::Get().FindTConsoleVariableDataInt(Name);
}

// Port of GetMappingPolynomial (PostProcessCombineLUTs.cpp lines ~113-127).
FVector3f GetTonemapMasterMappingPolynomial()
{
	static const TConsoleVariableData<float>* CVarColorMin = FindCVarFloat(TEXT("r.Color.Min"));
	static const TConsoleVariableData<float>* CVarColorMid = FindCVarFloat(TEXT("r.Color.Mid"));
	static const TConsoleVariableData<float>* CVarColorMax = FindCVarFloat(TEXT("r.Color.Max"));

	const float MinValue = FMath::Clamp(CVarColorMin ? CVarColorMin->GetValueOnRenderThread() : 0.0f, -10.0f, 10.0f);
	const float MidValue = FMath::Clamp(CVarColorMid ? CVarColorMid->GetValueOnRenderThread() : 0.5f, -10.0f, 10.0f);
	const float MaxValue = FMath::Clamp(CVarColorMax ? CVarColorMax->GetValueOnRenderThread() : 1.0f, -10.0f, 10.0f);

	// x is the input value, y the output value
	// RGB = a, b, c where y = a * x*x + b * x + c
	const float c = MinValue;
	const float b = 4 * MidValue - 3 * MinValue - MaxValue;
	const float a = MaxValue - MinValue - b;

	return FVector3f(a, b, c);
}

// Port of GenerateFinalTable (PostProcessCombineLUTs.cpp lines ~634-731).
uint32 GenerateFinalTable(const FFinalPostProcessSettings& Settings, const FTexture* OutTextures[], float OutWeights[], uint32 MaxCount)
{
	// Find the n strongest contributors, drop small contributors.
	uint32 LocalCount = 1;

	// Add the neutral one (done in the shader) as it should be the first and always there.
	OutTextures[0] = nullptr;
	OutWeights[0] = 0.0f;

	// Neutral index is the entry with no LUT texture assigned.
	for (int32 Index = 0; Index < Settings.ContributingLUTs.Num(); ++Index)
	{
		if (Settings.ContributingLUTs[Index].LUTTexture == nullptr)
		{
			OutWeights[0] = Settings.ContributingLUTs[Index].Weight;
			break;
		}
	}

	float OutWeightsSum = OutWeights[0];
	for (; LocalCount < MaxCount; ++LocalCount)
	{
		int32 BestIndex = INDEX_NONE;

		// Find the one with the strongest weight, add until full.
		for (int32 InputIndex = 0; InputIndex < Settings.ContributingLUTs.Num(); ++InputIndex)
		{
			bool bAlreadyInArray = false;

			{
				UTexture* LUTTexture = Settings.ContributingLUTs[InputIndex].LUTTexture;
				FTexture* Internal = LUTTexture ? LUTTexture->GetResource() : nullptr;
				for (uint32 OutputIndex = 0; OutputIndex < LocalCount; ++OutputIndex)
				{
					if (Internal == OutTextures[OutputIndex])
					{
						bAlreadyInArray = true;
						break;
					}
				}
			}

			if (bAlreadyInArray)
			{
				// We already have this one.
				continue;
			}

			// Take the first or better entry.
			if (BestIndex == INDEX_NONE || Settings.ContributingLUTs[BestIndex].Weight <= Settings.ContributingLUTs[InputIndex].Weight)
			{
				BestIndex = InputIndex;
			}
		}

		if (BestIndex == INDEX_NONE)
		{
			// No more elements to process.
			break;
		}

		const float WeightThreshold = 1.0f / 512.0f;

		const float BestWeight = Settings.ContributingLUTs[BestIndex].Weight;

		if (BestWeight < WeightThreshold)
		{
			// Drop small contributor
			break;
		}

		UTexture* BestLUTTexture = Settings.ContributingLUTs[BestIndex].LUTTexture;
		FTexture* BestInternal = BestLUTTexture ? BestLUTTexture->GetResource() : nullptr;

		OutTextures[LocalCount] = BestInternal;
		OutWeights[LocalCount] = BestWeight;
		OutWeightsSum += BestWeight;
	}

	// Normalize the weights.
	if (OutWeightsSum > 0.001f)
	{
		const float OutWeightsSumInverse = 1.0f / OutWeightsSum;

		for (uint32 Index = 0; Index < LocalCount; ++Index)
		{
			OutWeights[Index] *= OutWeightsSumInverse;
		}
	}
	else
	{
		// Just the neutral texture at full weight.
		OutWeights[0] = 1.0f;
		LocalCount = 1;
	}

	return LocalCount;
}

// POD snapshot of every input that affects the LUT contents; hashed to decide
// whether the persistent LUT needs a rebuild (see file-header comment).
struct FTonemapMasterLUTKey
{
	FVector4f OverlayColor;
	FVector3f ColorScale;
	FVector3f MappingPolynomial;

	FVector4f ColorSaturation;
	FVector4f ColorContrast;
	FVector4f ColorGamma;
	FVector4f ColorGain;
	FVector4f ColorOffset;
	FVector4f ColorSaturationShadows;
	FVector4f ColorContrastShadows;
	FVector4f ColorGammaShadows;
	FVector4f ColorGainShadows;
	FVector4f ColorOffsetShadows;
	FVector4f ColorSaturationMidtones;
	FVector4f ColorContrastMidtones;
	FVector4f ColorGammaMidtones;
	FVector4f ColorGainMidtones;
	FVector4f ColorOffsetMidtones;
	FVector4f ColorSaturationHighlights;
	FVector4f ColorContrastHighlights;
	FVector4f ColorGammaHighlights;
	FVector4f ColorGainHighlights;
	FVector4f ColorOffsetHighlights;

	float ColorCorrectionShadowsMax;
	float ColorCorrectionHighlightsMin;
	float ColorCorrectionHighlightsMax;

	float WhiteTemp;
	float WhiteTint;
	uint32 bIsTemperatureWhiteBalance;

	float BlueCorrection;
	float ExpandGamut;
	float ToneCurveAmount;
	uint32 TonemappingSDRInvEOTF;

	FVector3f InverseGamma;
	uint32 OutputDevice;
	uint32 OutputGamut;
	float OutputMaxLuminance;
	uint32 LUTShaper;

	int32 LUTSize;
	uint32 BlendCount;
	float LUTWeights[GTonemapMasterMaxLUTBlendCount];
	uint64 LUTTexturePtrs[GTonemapMasterMaxLUTBlendCount];

	uint32 LookMode;
	uint32 ContrastMode;
	uint32 TonemapperMode;
	float GT7TargetLuminance;
	float GT7BlendRatio;
	float GT7CurveMidPoint;
	float GT7CurveLinearSection;
	float GT7CurveToeStrength;
	float GT7CurveAlpha;
	float GT7ChromaFadeStart;
	float GT7ChromaFadeEnd;

	FMatrix44f WorkingToXYZ;
	FMatrix44f WorkingFromXYZ;
	FMatrix44f WorkingToAP1;
	FMatrix44f WorkingFromAP1;
	FMatrix44f WorkingToAP0;
	uint32 WorkingIsSRGB;

	uint32 ShaderPlatform;
};

class FTonemapMasterCombineLUTsCS : public FGlobalShader
{
public:
	static constexpr int32 GroupSize = 4;

	class FBlendCount : SHADER_PERMUTATION_RANGE_INT("BLENDCOUNT", 1, 5);
	class FOutputDeviceSRGB : SHADER_PERMUTATION_BOOL("OUTPUT_DEVICE_SRGB");
	using FPermutationDomain = TShaderPermutationDomain<FBlendCount, FOutputDeviceSRGB>;

	DECLARE_GLOBAL_SHADER(FTonemapMasterCombineLUTsCS);
	SHADER_USE_PARAMETER_STRUCT(FTonemapMasterCombineLUTsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FTonemapMasterCombineLUTParameters, CombineLUT)
		SHADER_PARAMETER(float, InvLUTSizeMinusOne)
		SHADER_PARAMETER(uint32, LookMode)
		SHADER_PARAMETER(uint32, ContrastMode)
		SHADER_PARAMETER(uint32, TonemapperMode)      // 0 = AgX, 1 = GT7 Color Volume Mapping
		SHADER_PARAMETER(FVector4f, GT7_Param)        // x=targetLuminance(nits), y=blendRatio, z=sdrCorrectionFactor, w=preExposure(always 1)
		SHADER_PARAMETER(FVector4f, GT7_Curve)        // x=midPoint, y=linearSection, z=toeStrength, w=alpha
		SHADER_PARAMETER(FVector4f, GT7_Chroma)       // x=fadeStart, y=fadeEnd, z/w unused
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, AgXContrastLUT)
		SHADER_PARAMETER_SAMPLER(SamplerState, AgXContrastLUTSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, InnerLUT)
		SHADER_PARAMETER_SAMPLER(SamplerState, InnerLUTSampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWOutputTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static EShaderPermutationPrecacheRequest ShouldPrecachePermutation(const FShaderPermutationParameters& Parameters)
	{
		// Matches the engine's FLUTBlenderCS: too many blends to precache.
		return EShaderPermutationPrecacheRequest::NotPrecached;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FTonemapMasterCombineLUTsCS, "/Plugin/TonemapMaster/TonemapMasterCombineLUTs.usf", "MainCS", SF_Compute);

} //! namespace

FTonemapMasterOutputDeviceParameters GetTonemapMasterOutputDeviceParameters(const FSceneViewFamily& Family)
{
	// Port of GetTonemapperOutputDeviceParameters (Renderer Private
	// PostProcessTonemap.cpp lines ~246-294), SDR-simplified: scene-capture HDR
	// modes and r.HDR.Display.OverrideOSMaxLuminance are not handled because the
	// scene view extension refuses HDR outputs upstream.
	EDisplayOutputFormat OutputDeviceValue = EDisplayOutputFormat::SDR_sRGB;
	if (Family.RenderTarget)
	{
		OutputDeviceValue = Family.RenderTarget->GetDisplayOutputFormat();
	}

	static const TConsoleVariableData<float>* CVarTonemapperGamma = FindCVarFloat(TEXT("r.TonemapperGamma"));
	float Gamma = CVarTonemapperGamma ? CVarTonemapperGamma->GetValueOnRenderThread() : 0.0f;

	// In case gamma is unspecified, fall back to 2.2 which is the most common case
	if ((PLATFORM_APPLE || OutputDeviceValue == EDisplayOutputFormat::SDR_ExplicitGammaMapping) && Gamma == 0.0f)
	{
		Gamma = 2.2f;
	}

	// Enforce user-controlled ramp over sRGB or Rec709
	if (Gamma > 0.0f && (OutputDeviceValue == EDisplayOutputFormat::SDR_sRGB || OutputDeviceValue == EDisplayOutputFormat::SDR_Rec709))
	{
		OutputDeviceValue = EDisplayOutputFormat::SDR_ExplicitGammaMapping;
	}

	const float DisplayGamma = Family.RenderTarget ? Family.RenderTarget->GetDisplayGamma() : 2.2f;

	FVector3f InvDisplayGammaValue;
	InvDisplayGammaValue.X = 1.0f / DisplayGamma;
	// HDR_LinearWithToneCurve (which uses 1.f here in the engine) cannot occur on the SDR-only path.
	InvDisplayGammaValue.Y = 2.2f / DisplayGamma;
	InvDisplayGammaValue.Z = 1.0f / FMath::Max(Gamma, 1.0f);

	static const TConsoleVariableData<int32>* CVarLUTShaper = FindCVarInt(TEXT("r.LUT.Shaper"));

	FTonemapMasterOutputDeviceParameters Parameters;
	Parameters.InverseGamma = InvDisplayGammaValue;
	Parameters.OutputDevice = static_cast<uint32>(OutputDeviceValue);
	Parameters.OutputGamut = Family.RenderTarget ? static_cast<uint32>(Family.RenderTarget->GetDisplayColorGamut()) : 0u;
	Parameters.OutputMaxLuminance = Family.RenderTarget ? Family.RenderTarget->GetMaximumLuminanceInNits() : 100.f;
	Parameters.LUTShaper = CVarLUTShaper ? static_cast<uint32>(CVarLUTShaper->GetValueOnRenderThread()) : 0u;
	return Parameters;
}

float GetTonemapMasterLUTMax(const FTonemapMasterOutputDeviceParameters& Parameters)
{
	// Port of GetTonemappingMax (PostProcessCombineLUTs.cpp lines ~327-341),
	// Filmic tonemapping method only (HDR cases excluded by the SDR guard).
	float LUTMax = 1.f;
	switch (static_cast<EDisplayOutputFormat>(Parameters.OutputDevice))
	{
	case EDisplayOutputFormat::SDR_sRGB:
	case EDisplayOutputFormat::SDR_Rec709:
		LUTMax = 1.05f;
		break;
	default:
		break;
	}
	return LUTMax;
}

FRDGTextureRef AddTonemapMasterCombineLUTPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FTonemapMasterGradingLUTCache& Cache,
	FTextureRHIRef AgXContrastLUTRHI,
	uint32 LookMode,
	uint32 ContrastMode,
	uint32 TonemapperMode,
	const FTonemapMasterGT7Settings& GT7Settings)
{
	const FSceneViewFamily& ViewFamily = *View.Family;

	// Gather the contributing color grading LUTs (engine: AddCombineLUTPass
	// lines ~738-750).
	const FTexture* LocalTextures[GTonemapMasterMaxLUTBlendCount] = {};
	float LocalWeights[GTonemapMasterMaxLUTBlendCount] = {};
	uint32 LocalCount = 1;

	// Default to no LUTs.
	LocalTextures[0] = nullptr;
	LocalWeights[0] = 1.0f;

	if (ViewFamily.EngineShowFlags.ColorGrading)
	{
		LocalCount = GenerateFinalTable(View.FinalPostProcessSettings, LocalTextures, LocalWeights, GTonemapMasterMaxLUTBlendCount);
	}

	// Effective settings (engine: lines ~766-769).
	static const TConsoleVariableData<int32>* CVarColorGrading = FindCVarInt(TEXT("r.Color.Grading"));
	const bool bColorGrading = ViewFamily.EngineShowFlags.ColorGrading
		&& (!CVarColorGrading || CVarColorGrading->GetValueOnRenderThread() != 0);
	const FPostProcessSettings& Settings = bColorGrading
		? static_cast<const FPostProcessSettings&>(View.FinalPostProcessSettings)
		: FPostProcessSettings::GetDefault();

	static const TConsoleVariableData<int32>* CVarLUTSize = FindCVarInt(TEXT("r.LUT.Size"));
	static const TConsoleVariableData<int32>* CVarLUTInnerSize = FindCVarInt(TEXT("r.LUT.Inner.Size"));
	const int32 LUTSize = CVarLUTSize ? CVarLUTSize->GetValueOnRenderThread() : 32;
	const int32 InnerLUTSize = CVarLUTInnerSize ? CVarLUTInnerSize->GetValueOnRenderThread() : 64;

	const FTonemapMasterOutputDeviceParameters OutputDeviceParams = GetTonemapMasterOutputDeviceParameters(ViewFamily);
	const float LUTMax = GetTonemapMasterLUTMax(OutputDeviceParams);

	// TonemappingSDRInvEOTF (engine: lines ~458-462). The engine default depends
	// on the tonemapping method (StandardACES -> BT.1886); our LUT builder only
	// supports the Filmic path, whose default is sRGB (0).
	static const TConsoleVariableData<int32>* CVarTonemappingSDRInvEOTF = FindCVarInt(TEXT("r.LUT.TonemappingSDRInvEOTF"));
	const int32 TonemappingSDRInvEOTFOverride = CVarTonemappingSDRInvEOTF ? CVarTonemappingSDRInvEOTF->GetValueOnRenderThread() : -1;
	const uint32 TonemappingSDRInvEOTF = (TonemappingSDRInvEOTFOverride >= 0)
		? static_cast<uint32>(TonemappingSDRInvEOTFOverride)
		: 0u;

	// Hash every LUT-affecting input (replaces the engine's FCachedLUTSettings
	// field-by-field change detection, PostProcessCombineLUTs.cpp ~343-470).
	FTonemapMasterLUTKey Key;
	FMemory::Memzero(Key);
	{
		Key.OverlayColor = FVector4f(View.OverlayColor);
		Key.ColorScale = FVector3f(View.ColorScale);
		Key.MappingPolynomial = GetTonemapMasterMappingPolynomial();

		Key.ColorSaturation = FVector4f(Settings.ColorSaturation);
		Key.ColorContrast = FVector4f(Settings.ColorContrast);
		Key.ColorGamma = FVector4f(Settings.ColorGamma);
		Key.ColorGain = FVector4f(Settings.ColorGain);
		Key.ColorOffset = FVector4f(Settings.ColorOffset);
		Key.ColorSaturationShadows = FVector4f(Settings.ColorSaturationShadows);
		Key.ColorContrastShadows = FVector4f(Settings.ColorContrastShadows);
		Key.ColorGammaShadows = FVector4f(Settings.ColorGammaShadows);
		Key.ColorGainShadows = FVector4f(Settings.ColorGainShadows);
		Key.ColorOffsetShadows = FVector4f(Settings.ColorOffsetShadows);
		Key.ColorSaturationMidtones = FVector4f(Settings.ColorSaturationMidtones);
		Key.ColorContrastMidtones = FVector4f(Settings.ColorContrastMidtones);
		Key.ColorGammaMidtones = FVector4f(Settings.ColorGammaMidtones);
		Key.ColorGainMidtones = FVector4f(Settings.ColorGainMidtones);
		Key.ColorOffsetMidtones = FVector4f(Settings.ColorOffsetMidtones);
		Key.ColorSaturationHighlights = FVector4f(Settings.ColorSaturationHighlights);
		Key.ColorContrastHighlights = FVector4f(Settings.ColorContrastHighlights);
		Key.ColorGammaHighlights = FVector4f(Settings.ColorGammaHighlights);
		Key.ColorGainHighlights = FVector4f(Settings.ColorGainHighlights);
		Key.ColorOffsetHighlights = FVector4f(Settings.ColorOffsetHighlights);

		Key.ColorCorrectionShadowsMax = Settings.ColorCorrectionShadowsMax;
		Key.ColorCorrectionHighlightsMin = Settings.ColorCorrectionHighlightsMin;
		Key.ColorCorrectionHighlightsMax = Settings.ColorCorrectionHighlightsMax;

		Key.WhiteTemp = Settings.WhiteTemp;
		Key.WhiteTint = Settings.WhiteTint;
		Key.bIsTemperatureWhiteBalance = uint32(Settings.TemperatureType == ETemperatureMethod::TEMP_WhiteBalance);

		Key.BlueCorrection = Settings.BlueCorrection;
		Key.ExpandGamut = Settings.ExpandGamut;
		Key.ToneCurveAmount = Settings.ToneCurveAmount;
		Key.TonemappingSDRInvEOTF = TonemappingSDRInvEOTF;

		Key.InverseGamma = OutputDeviceParams.InverseGamma;
		Key.OutputDevice = OutputDeviceParams.OutputDevice;
		Key.OutputGamut = OutputDeviceParams.OutputGamut;
		Key.OutputMaxLuminance = OutputDeviceParams.OutputMaxLuminance;
		Key.LUTShaper = OutputDeviceParams.LUTShaper;

		Key.LUTSize = LUTSize;
		Key.BlendCount = LocalCount;
		for (uint32 BlendIndex = 0; BlendIndex < GTonemapMasterMaxLUTBlendCount; ++BlendIndex)
		{
			Key.LUTWeights[BlendIndex] = LocalWeights[BlendIndex];
			Key.LUTTexturePtrs[BlendIndex] = (LocalTextures[BlendIndex] && LocalTextures[BlendIndex]->TextureRHI)
				? reinterpret_cast<uint64>(LocalTextures[BlendIndex]->TextureRHI.GetReference())
				: 0;
		}

		Key.LookMode = LookMode;
		Key.ContrastMode = ContrastMode;
		Key.TonemapperMode = TonemapperMode;
		Key.GT7TargetLuminance = GT7Settings.TargetLuminance;
		Key.GT7BlendRatio = GT7Settings.BlendRatio;
		Key.GT7CurveMidPoint = GT7Settings.CurveMidPoint;
		Key.GT7CurveLinearSection = GT7Settings.CurveLinearSection;
		Key.GT7CurveToeStrength = GT7Settings.CurveToeStrength;
		Key.GT7CurveAlpha = GT7Settings.CurveAlpha;
		Key.GT7ChromaFadeStart = GT7Settings.ChromaFadeStart;
		Key.GT7ChromaFadeEnd = GT7Settings.ChromaFadeEnd;

		// Working color space contents (engine: FCachedLUTSettings::UpdateCachedValues ~362-371).
		const FWorkingColorSpaceShaderParameters* WorkingColorSpaceParams =
			reinterpret_cast<const FWorkingColorSpaceShaderParameters*>(GDefaultWorkingColorSpaceUniformBuffer.GetContents());
		if (WorkingColorSpaceParams)
		{
			Key.WorkingToXYZ = WorkingColorSpaceParams->ToXYZ;
			Key.WorkingFromXYZ = WorkingColorSpaceParams->FromXYZ;
			Key.WorkingToAP1 = WorkingColorSpaceParams->ToAP1;
			Key.WorkingFromAP1 = WorkingColorSpaceParams->FromAP1;
			Key.WorkingToAP0 = WorkingColorSpaceParams->ToAP0;
			Key.WorkingIsSRGB = WorkingColorSpaceParams->bIsSRGB;
		}

		Key.ShaderPlatform = static_cast<uint32>(View.GetShaderPlatform());
	}
	const uint64 SettingsHash = CityHash64(reinterpret_cast<const char*>(&Key), sizeof(Key));

	// LUT pixel format (engine: FSceneViewState::CreateLUTRenderTarget
	// lines ~136-165). bNeedFloatOutput is always false here because the plugin
	// is SDR-only.
	EPixelFormat LUTFormat = PF_A2B10G10R10;
	if (!UE::PixelFormat::HasCapabilities(LUTFormat, EPixelFormatCapabilities::UAV))
	{
		LUTFormat = PF_R8G8B8A8;
	}

	// (Re)create the persistent volume texture when needed.
	if (!Cache.TextureRHI.IsValid() || Cache.LUTSize != LUTSize || Cache.Format != LUTFormat)
	{
		const FRHITextureCreateDesc CreateDesc =
			FRHITextureCreateDesc::Create3D(TEXT("TonemapMaster.GradingLUT"), FIntVector(LUTSize, LUTSize, LUTSize), LUTFormat)
			.SetFlags(TexCreate_ShaderResource | TexCreate_UAV)
			.SetInitialState(ERHIAccess::SRVGraphics);

		Cache.TextureRHI = FRHICommandListImmediate::Get().CreateTexture(CreateDesc);
		Cache.LUTSize = LUTSize;
		Cache.Format = LUTFormat;
		Cache.SettingsHash = 0; // contents are undefined; force a rebuild below
	}

	FRDGTextureRef OutputTexture = GraphBuilder.RegisterExternalTexture(
		CreateRenderTarget(Cache.TextureRHI, TEXT("TonemapMaster.GradingLUT")),
		ERDGTextureFlags::MultiFrame);

	// Cache hit: the persistent texture already holds the LUT for these settings.
	if (SettingsHash == Cache.SettingsHash)
	{
		return OutputTexture;
	}

	// Fill the shader parameters (port of
	// FCachedLUTSettings::GetCombineLUTParameters, lines ~376-469).
	FTonemapMasterCombineLUTsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FTonemapMasterCombineLUTsCS::FParameters>();
	FTonemapMasterCombineLUTParameters& CombineLUT = PassParameters->CombineLUT;
	{
		for (uint32 BlendIndex = 1; BlendIndex < LocalCount; ++BlendIndex)
		{
			check(LocalTextures[BlendIndex]);

			// Don't use texture asset sampler as it might have anisotropic filtering enabled
			CombineLUT.Textures[BlendIndex] = LocalTextures[BlendIndex]->TextureRHI;
			CombineLUT.Samplers[BlendIndex] = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp, 0, 1>::GetRHI();
		}
		for (uint32 BlendIndex = 0; BlendIndex < LocalCount; ++BlendIndex)
		{
			GET_SCALAR_ARRAY_ELEMENT(CombineLUT.LUTWeights, BlendIndex) = LocalWeights[BlendIndex];
		}

		CombineLUT.WorkingColorSpace = GDefaultWorkingColorSpaceUniformBuffer.GetUniformBufferRef();
		CombineLUT.OutputDevice = OutputDeviceParams;

		CombineLUT.ColorScale = Key.ColorScale;
		CombineLUT.OverlayColor = Key.OverlayColor;
		CombineLUT.ColorRemap.MappingPolynomial = Key.MappingPolynomial;

		CombineLUT.bIsTemperatureWhiteBalance = Key.bIsTemperatureWhiteBalance;
		CombineLUT.LUTInvMax = 1.f / LUTMax;
		CombineLUT.LUTSize = static_cast<float>(LUTSize);
		CombineLUT.InnerLUTSize = static_cast<float>(InnerLUTSize);
		CombineLUT.WhiteTemp = Settings.WhiteTemp;
		CombineLUT.WhiteTint = Settings.WhiteTint;

		CombineLUT.ColorSaturation = Key.ColorSaturation;
		CombineLUT.ColorContrast = Key.ColorContrast;
		CombineLUT.ColorGamma = Key.ColorGamma;
		CombineLUT.ColorGain = Key.ColorGain;
		CombineLUT.ColorOffset = Key.ColorOffset;
		CombineLUT.ColorSaturationShadows = Key.ColorSaturationShadows;
		CombineLUT.ColorContrastShadows = Key.ColorContrastShadows;
		CombineLUT.ColorGammaShadows = Key.ColorGammaShadows;
		CombineLUT.ColorGainShadows = Key.ColorGainShadows;
		CombineLUT.ColorOffsetShadows = Key.ColorOffsetShadows;
		CombineLUT.ColorSaturationMidtones = Key.ColorSaturationMidtones;
		CombineLUT.ColorContrastMidtones = Key.ColorContrastMidtones;
		CombineLUT.ColorGammaMidtones = Key.ColorGammaMidtones;
		CombineLUT.ColorGainMidtones = Key.ColorGainMidtones;
		CombineLUT.ColorOffsetMidtones = Key.ColorOffsetMidtones;
		CombineLUT.ColorSaturationHighlights = Key.ColorSaturationHighlights;
		CombineLUT.ColorContrastHighlights = Key.ColorContrastHighlights;
		CombineLUT.ColorGammaHighlights = Key.ColorGammaHighlights;
		CombineLUT.ColorGainHighlights = Key.ColorGainHighlights;
		CombineLUT.ColorOffsetHighlights = Key.ColorOffsetHighlights;
		CombineLUT.ColorCorrectionShadowsMax = Settings.ColorCorrectionShadowsMax;
		CombineLUT.ColorCorrectionHighlightsMin = Settings.ColorCorrectionHighlightsMin;
		CombineLUT.ColorCorrectionHighlightsMax = Settings.ColorCorrectionHighlightsMax;
		CombineLUT.BlueCorrection = Settings.BlueCorrection;
		CombineLUT.ExpandGamut = Settings.ExpandGamut;
		CombineLUT.ToneCurveAmount = Settings.ToneCurveAmount;
		CombineLUT.TonemappingSDRInvEOTF = TonemappingSDRInvEOTF;
	}

	PassParameters->InvLUTSizeMinusOne = 1.f / (LUTSize - 1);
	PassParameters->LookMode = LookMode;
	PassParameters->ContrastMode = ContrastMode;
	PassParameters->TonemapperMode = TonemapperMode;
	{
		// SDR: resolve the target to the 100-nit reference so the GT7 curve's
		// peakIntensity is 1.0 in frame-buffer space (see header note).
		// HDR: use the user target luminance. sdrCorrectionFactor stays 1.0.
		const bool bIsHDROutput = OutputDeviceParams.OutputDevice >= (uint32)EDisplayOutputFormat::HDR_ACES_1000nit_ST2084;
		const float TargetNits = bIsHDROutput ? GT7Settings.TargetLuminance : 100.0f;

		// preExposure MUST stay 1.0: the LUT is a pure color transform; scene
		// exposure is applied outside the LUT by the tonemap pass.
		PassParameters->GT7_Param = FVector4f(TargetNits, GT7Settings.BlendRatio, 1.0f, 1.0f);
		PassParameters->GT7_Curve = FVector4f(GT7Settings.CurveMidPoint, GT7Settings.CurveLinearSection, GT7Settings.CurveToeStrength, GT7Settings.CurveAlpha);
		PassParameters->GT7_Chroma = FVector4f(GT7Settings.ChromaFadeStart, GT7Settings.ChromaFadeEnd, 0.0f, 0.0f);
	}
	PassParameters->AgXContrastLUT = GraphBuilder.RegisterExternalTexture(
		CreateRenderTarget(AgXContrastLUTRHI, TEXT("TonemapMaster.AgXContrastLUT")),
		ERDGTextureFlags::MultiFrame);
	PassParameters->AgXContrastLUTSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	// The inner LUT is only consumed by the ACES / automatic-HDR paths that
	// cannot trigger on SDR outputs; bind the engine's black volume fallback
	// (engine: AddCombineLUTPass lines ~836-838).
	PassParameters->InnerLUT = RegisterExternalTexture(
		GraphBuilder, GBlackVolumeTexture->GetTextureRHI(), TEXT("TonemapMaster.InnerLUT_Fallback"));
	PassParameters->InnerLUTSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->RWOutputTexture = GraphBuilder.CreateUAV(OutputTexture);

	FTonemapMasterCombineLUTsCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FTonemapMasterCombineLUTsCS::FBlendCount>(LocalCount);
	PermutationVector.Set<FTonemapMasterCombineLUTsCS::FOutputDeviceSRGB>(
		OutputDeviceParams.OutputDevice == static_cast<uint32>(EDisplayOutputFormat::SDR_sRGB));

	TShaderMapRef<FTonemapMasterCombineLUTsCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()), PermutationVector);

	const uint32 GroupCount = FMath::DivideAndRoundUp(LUTSize, FTonemapMasterCombineLUTsCS::GroupSize);
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("TonemapMaster.CombineLUTs %d (CS)", LUTSize),
		ERDGPassFlags::Compute,
		ComputeShader,
		PassParameters,
		FIntVector(GroupCount, GroupCount, GroupCount));

	Cache.SettingsHash = SettingsHash;
	return OutputTexture;
}
