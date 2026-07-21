// Copyright (c) 2026. TonemapMaster plugin.
//
// Pass A — AgX grading-LUT builder. Port of the engine's
//   Source/Runtime/Renderer/Private/PostProcess/PostProcessCombineLUTs.{h,cpp}
// adapted to FSceneView-only inputs (FViewInfo and the Renderer-private
// helpers are not accessible from a plugin).

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "SceneManagement.h"
#include "ShaderParameterStruct.h"

class FSceneView;
class FSceneViewFamily;

// Including the neutral one at index 0 (engine: PostProcessCombineLUTs.cpp GMaxLUTBlendCount).
static constexpr uint32 GTonemapMasterMaxLUTBlendCount = 5;

// Port of engine FColorRemapParameters (PostProcessCombineLUTs.h).
BEGIN_SHADER_PARAMETER_STRUCT(FTonemapMasterColorRemapParameters, )
	SHADER_PARAMETER(FVector3f, MappingPolynomial)
END_SHADER_PARAMETER_STRUCT()

// Port of engine FTonemapperOutputDeviceParameters (Renderer Private
// PostProcess/PostProcessTonemap.h). Same member names/layout so the shader
// bindings match the ported shader code.
BEGIN_SHADER_PARAMETER_STRUCT(FTonemapMasterOutputDeviceParameters, )
	SHADER_PARAMETER(FVector3f, InverseGamma)
	SHADER_PARAMETER(uint32, OutputDevice)
	SHADER_PARAMETER(uint32, OutputGamut)
	SHADER_PARAMETER(float, OutputMaxLuminance)
	SHADER_PARAMETER(uint32, LUTShaper)
END_SHADER_PARAMETER_STRUCT()

// Port of engine FCombineLUTParameters (PostProcessCombineLUTs.cpp), minus:
//  * LimitingRgbToXYZ*/LimitingXYZToRgb* — declared but never used by the
//    combine-LUT shader (only the ACES inner-LUT shader uses them, which we
//    do not port);
//  * FilmSlope/FilmToe/FilmShoulder/FilmBlackClip/FilmWhiteClip — only read
//    by FilmToneMap(), which this plugin replaces with AgX.
BEGIN_SHADER_PARAMETER_STRUCT(FTonemapMasterCombineLUTParameters, )
	SHADER_PARAMETER_TEXTURE_ARRAY(Texture2D, Textures, [GTonemapMasterMaxLUTBlendCount])
	SHADER_PARAMETER_SAMPLER_ARRAY(SamplerState, Samplers, [GTonemapMasterMaxLUTBlendCount])
	SHADER_PARAMETER_SCALAR_ARRAY(float, LUTWeights, [GTonemapMasterMaxLUTBlendCount])
	SHADER_PARAMETER_STRUCT_REF(FWorkingColorSpaceShaderParameters, WorkingColorSpace)
	SHADER_PARAMETER(FVector4f, OverlayColor)
	SHADER_PARAMETER(FVector3f, ColorScale)
	SHADER_PARAMETER(FVector4f, ColorSaturation)
	SHADER_PARAMETER(FVector4f, ColorContrast)
	SHADER_PARAMETER(FVector4f, ColorGamma)
	SHADER_PARAMETER(FVector4f, ColorGain)
	SHADER_PARAMETER(FVector4f, ColorOffset)
	SHADER_PARAMETER(FVector4f, ColorSaturationShadows)
	SHADER_PARAMETER(FVector4f, ColorContrastShadows)
	SHADER_PARAMETER(FVector4f, ColorGammaShadows)
	SHADER_PARAMETER(FVector4f, ColorGainShadows)
	SHADER_PARAMETER(FVector4f, ColorOffsetShadows)
	SHADER_PARAMETER(FVector4f, ColorSaturationMidtones)
	SHADER_PARAMETER(FVector4f, ColorContrastMidtones)
	SHADER_PARAMETER(FVector4f, ColorGammaMidtones)
	SHADER_PARAMETER(FVector4f, ColorGainMidtones)
	SHADER_PARAMETER(FVector4f, ColorOffsetMidtones)
	SHADER_PARAMETER(FVector4f, ColorSaturationHighlights)
	SHADER_PARAMETER(FVector4f, ColorContrastHighlights)
	SHADER_PARAMETER(FVector4f, ColorGammaHighlights)
	SHADER_PARAMETER(FVector4f, ColorGainHighlights)
	SHADER_PARAMETER(FVector4f, ColorOffsetHighlights)
	SHADER_PARAMETER(float, LUTInvMax)
	SHADER_PARAMETER(float, LUTSize)
	SHADER_PARAMETER(float, InnerLUTSize)
	SHADER_PARAMETER(float, WhiteTemp)
	SHADER_PARAMETER(float, WhiteTint)
	SHADER_PARAMETER(float, ColorCorrectionShadowsMax)
	SHADER_PARAMETER(float, ColorCorrectionHighlightsMin)
	SHADER_PARAMETER(float, ColorCorrectionHighlightsMax)
	SHADER_PARAMETER(float, BlueCorrection)
	SHADER_PARAMETER(float, ExpandGamut)
	SHADER_PARAMETER(float, ToneCurveAmount)
	SHADER_PARAMETER(uint32, TonemappingSDRInvEOTF)
	SHADER_PARAMETER(uint32, bIsTemperatureWhiteBalance)
	SHADER_PARAMETER_STRUCT_INCLUDE(FTonemapMasterColorRemapParameters, ColorRemap)
	SHADER_PARAMETER_STRUCT_INCLUDE(FTonemapMasterOutputDeviceParameters, OutputDevice)
END_SHADER_PARAMETER_STRUCT()

// Persistent state for the plugin-managed grading LUT volume texture. Owned
// by the scene view extension (one instance, shared across views; see the
// caching notes in TonemapMasterCombineLUTs.cpp).
struct FTonemapMasterGradingLUTCache
{
	// Persistent 3D RHI texture (LUTSize^3). Recreated when the size changes.
	FTextureRHIRef TextureRHI;

	// CityHash64 of every input that affects the LUT contents. The compute
	// pass only re-runs when this changes.
	uint64 SettingsHash = 0;

	int32 LUTSize = 0;
	EPixelFormat Format = PF_Unknown;
};

// Port of engine GetTonemapperOutputDeviceParameters (Renderer Private
// PostProcessTonemap.cpp lines ~246-294), SDR-simplified: the scene view
// extension refuses HDR outputs upstream, so scene-capture HDR modes and the
// HDR luminance override CVar are not handled.
FTonemapMasterOutputDeviceParameters GetTonemapMasterOutputDeviceParameters(const FSceneViewFamily& Family);

// Port of engine GetTonemappingMax (PostProcessCombineLUTs.cpp lines ~327-341),
// hardcoded to the Filmic tonemapping method (the plugin's LUT builder only
// supports that path). Used for LUTInvMax when building the LUT and for
// LUTMax when sampling it per-pixel.
float GetTonemapMasterLUTMax(const FTonemapMasterOutputDeviceParameters& Parameters);

// Builds (or returns the cached) 3D grading LUT volume texture for this view,
// dispatching the TonemapMasterCombineLUTs compute pass when the inputs
// changed. Render thread only.
FRDGTextureRef AddTonemapMasterCombineLUTPass(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FTonemapMasterGradingLUTCache& Cache,
	FTextureRHIRef AgXContrastLUTRHI,
	uint32 LookMode,
	uint32 ContrastMode);
