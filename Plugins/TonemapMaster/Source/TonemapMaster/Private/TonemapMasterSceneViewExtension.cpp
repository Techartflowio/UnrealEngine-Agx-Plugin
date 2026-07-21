// Copyright (c) 2026. TonemapMaster plugin.

#include "TonemapMasterSceneViewExtension.h"

#include "AgXContrastLUT.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/Scene.h"
#include "Engine/Texture.h"
#include "GlobalShader.h"
#include "Math/Halton.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "RHIStaticStates.h"
#include "RHITextureInitializer.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"
#include "SystemTextures.h"
#include "TextureResource.h"
#include "TonemapMasterCombineLUTs.h"
#include "UnrealClient.h"

static TAutoConsoleVariable<int32> CVarTonemapMasterEnable(
	TEXT("r.TonemapMaster.Enable"),
	1,
	TEXT("Replace the engine filmic tonemapper with the TonemapMaster AgX tonemapper.\n")
	TEXT(" 0: engine tonemapper\n")
	TEXT(" 1: TonemapMaster AgX (default)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarTonemapMasterLook(
	TEXT("r.TonemapMaster.Look"),
	2,
	TEXT("AgX look applied after the sigmoid. Baked into the grading LUT by Pass A.\n")
	TEXT(" 0: Base\n")
	TEXT(" 1: Golden\n")
	TEXT(" 2: Punchy (default, matches the PostProcessCombineLUTs_5.7.usf reference)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarTonemapMasterContrastMode(
	TEXT("r.TonemapMaster.ContrastMode"),
	0,
	TEXT("Contrast curve source for the AgX sigmoid. Baked into the grading LUT by Pass A.\n")
	TEXT(" 0: reference 4096-entry LUT, bit-exact with the OCIO config (default)\n")
	TEXT(" 1: polynomial approximation from Benjamin Wrensch's minimal AgX\n")
	TEXT("    (https://iolite-engine.com/blog_posts/minimal_agx_implementation),\n")
	TEXT("    max error ~5.8e-3 near x=0.95, i.e. <1/255 in the extreme highlights"),
	ECVF_RenderThreadSafe);

// Pass B — per-pixel tonemap replacement: scene color + bloom -> exposure ->
// vignette -> 3D grading LUT (built by Pass A) -> output. Port of the engine's
// FTonemapPS desktop non-gamma-only path (PostProcessTonemap.cpp/usf), minus
// film grain, sharpen, local exposure, color fringe, bloom dirt mask, the
// mobile gamma-only path and the OutLuminance render target.
class FTonemapMasterPS : public FGlobalShader
{
public:
	class FAdvancedVignetteDim : SHADER_PERMUTATION_BOOL("USE_ADVANCED_VIGNETTE");
	class FVignetteTextureDim : SHADER_PERMUTATION_BOOL("USE_VIGNETTE_TEXTURE");
	using FPermutationDomain = TShaderPermutationDomain<FAdvancedVignetteDim, FVignetteTextureDim>;

	DECLARE_GLOBAL_SHADER(FTonemapMasterPS);
	SHADER_USE_PARAMETER_STRUCT(FTonemapMasterPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Color)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Output)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, Bloom)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ColorTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, ColorSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BloomTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, BloomSampler)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, EyeAdaptationBuffer)
		SHADER_PARAMETER(FScreenTransform, ColorToBloom)
		SHADER_PARAMETER(FVector4f, ColorScale0)
		SHADER_PARAMETER(FVector4f, VignetteParams0)
		SHADER_PARAMETER(FVector4f, VignetteParams1)
		SHADER_PARAMETER(float, VignetteParams2)
		SHADER_PARAMETER(uint32, VignetteType)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VignetteTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, VignetteTextureSampler)
		SHADER_PARAMETER(FVector4f, LensPrincipalPointOffsetScale)
		SHADER_PARAMETER(FVector4f, LensPrincipalPointOffsetScaleInverse)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D, ColorGradingLUT)
		SHADER_PARAMETER_SAMPLER(SamplerState, ColorGradingLUTSampler)
		SHADER_PARAMETER(uint32, LUTShaper)
		SHADER_PARAMETER(float, LUTMax)
		SHADER_PARAMETER(float, LUTScale)
		SHADER_PARAMETER(float, LUTOffset)
		SHADER_PARAMETER(float, BackbufferQuantizationDithering)
		SHADER_PARAMETER(FVector3f, GrainRandomFull)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FTonemapMasterPS, "/Plugin/TonemapMaster/TonemapMasterPS.usf", "MainPS", SF_Pixel);

FTonemapMasterSceneViewExtension::FTonemapMasterSceneViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

bool FTonemapMasterSceneViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return CVarTonemapMasterEnable.GetValueOnAnyThread() != 0;
}

FTextureRHIRef FTonemapMasterSceneViewExtension::GetOrCreateAgXContrastLUT_RenderThread()
{
	if (AgXContrastLUT.IsValid())
	{
		return AgXContrastLUT;
	}

	// Reference AgX contrast curve. Stored as a 1D LUT in R32_FLOAT for
	// bit-exact matching with the OCIO config; a polynomial approximation had
	// ~5.8e-3 max error in the shoulder, which is visible on highlights.
	//
	// We use a 1xN Texture2D rather than a true Texture1D because Texture1D is
	// not universally supported on all feature levels / RHI backends UE targets.
	// No SRGB flag: R32_FLOAT is a float format, SRGB is meaningless on it and
	// is rejected by some RHI backends.
	const FRHITextureCreateDesc CreateDesc =
		FRHITextureCreateDesc::Create2D(TEXT("TonemapMaster.AgXContrastLUT"), GAgXContrastLUTSize, 1, PF_R32_FLOAT)
		.SetFlags(TexCreate_ShaderResource)
		.SetInitialState(ERHIAccess::SRVGraphics)
		.SetInitActionInitializer();

	FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
	FRHITextureInitializer Initializer = RHICmdList.CreateTextureInitializer(CreateDesc);

	// R32_FLOAT = one float per texel; a single mip with no block compression,
	// so source stride == destination stride.
	const FRHITextureSubresourceInitializer Subresource = Initializer.GetTexture2DSubresource(0);
	const uint32 SourceBytes = GAgXContrastLUTSize * sizeof(float);
	FMemory::Memcpy(Subresource.Data, GAgXContrastLUT, SourceBytes);

	AgXContrastLUT = Initializer.Finalize();
	return AgXContrastLUT;
}

namespace
{
	// The AgX pipeline implemented here is SDR-only (BT.709 gamut, pure gamma
	// EOTF). HDR output devices (ST2084 / scRGB / linear HDR scene captures)
	// need a different outset gamut and OETF, so for those we stay unsubscribed
	// and let the engine's own ACES tonemapper run instead of producing
	// double-gamma-encoded, clipped output.
	bool IsSDRDisplayOutput(const FSceneView& InView)
	{
		const FSceneViewFamily* Family = InView.Family;
		if (!Family || !Family->RenderTarget)
		{
			return true;
		}
		if (Family->SceneCaptureSource == SCS_FinalColorHDR || Family->SceneCaptureSource == SCS_FinalToneCurveHDR)
		{
			return false;
		}
		const EDisplayOutputFormat OutputFormat = Family->RenderTarget->GetDisplayOutputFormat();
		return OutputFormat == EDisplayOutputFormat::SDR_sRGB
			|| OutputFormat == EDisplayOutputFormat::SDR_Rec709
			|| OutputFormat == EDisplayOutputFormat::SDR_ExplicitGammaMapping;
	}

	// Port of GrainRandomFromFrame (Renderer Private PostProcessTonemap.h).
	void GrainRandomFromFrame(FVector3f* RESTRICT const Constant, uint32 FrameNumber)
	{
		Constant->X = Halton(FrameNumber & 1023, 2);
		Constant->Y = Halton(FrameNumber & 1023, 3);
	}
}

void FTonemapMasterSceneViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView, FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	// bIsPassEnabled mirrors whether the engine tonemap pass would run for this view.
	if (Pass == EPostProcessingPass::ReplacingTonemapper && bIsPassEnabled && IsSDRDisplayOutput(InView))
	{
		InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FTonemapMasterSceneViewExtension::ReplaceTonemapper_RenderThread));
	}
}

FScreenPassTexture FTonemapMasterSceneViewExtension::ReplaceTonemapper_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
{
	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	check(SceneColor.IsValid());

	// Bloom is composited by the tonemapper in UE, so a replacing pass has to apply it itself.
	FScreenPassTexture Bloom;
	{
		const FScreenPassTextureSlice BloomSlice = Inputs.GetInput(EPostProcessMaterialInput::CombinedBloom);
		if (BloomSlice.IsValid())
		{
			Bloom = FScreenPassTexture::CopyFromSlice(GraphBuilder, BloomSlice);
		}
		else
		{
			Bloom = FScreenPassTexture(FRDGSystemTextures::Get(GraphBuilder).Black);
		}
	}

	// Write straight into the override output when we are the last pass in the
	// chain, otherwise allocate a display-referred LDR target like the engine
	// tonemapper does.
	FScreenPassRenderTarget Output = Inputs.OverrideOutput;
	if (!Output.IsValid())
	{
		FRDGTextureDesc OutputDesc = SceneColor.Texture->Desc;
		OutputDesc.Reset();
		OutputDesc.Format = PF_B8G8R8A8;
		OutputDesc.Flags |= TexCreate_RenderTargetable | TexCreate_ShaderResource;
		OutputDesc.ClearValue = FClearValueBinding(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));

		Output = FScreenPassRenderTarget(
			GraphBuilder.CreateTexture(OutputDesc, TEXT("TonemapMaster.SceneColor")),
			SceneColor.ViewRect,
			View.GetOverwriteLoadAction());
	}

	// Scene color is stored pre-exposed; the tonemapper is where eye adaptation
	// gets applied, so fetch the exposure buffer just like the engine pass does.
	FRDGBufferRef EyeAdaptationBuffer = nullptr;
	if (View.HasValidEyeAdaptationBuffer())
	{
		EyeAdaptationBuffer = GraphBuilder.RegisterExternalBuffer(View.GetEyeAdaptationBuffer(), ERDGBufferFlags::MultiFrame);
	}
	else
	{
		const FVector4f DefaultExposure(1.0f, 1.0f, 1.0f, 1.0f);
		EyeAdaptationBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("TonemapMaster.DefaultEyeAdaptation"), sizeof(FVector4f), 1, &DefaultExposure, sizeof(FVector4f));
	}

	const uint32 LookMode = (uint32)FMath::Clamp(CVarTonemapMasterLook.GetValueOnRenderThread(), 0, 2);
	const uint32 ContrastMode = (uint32)FMath::Clamp(CVarTonemapMasterContrastMode.GetValueOnRenderThread(), 0, 1);

	// ------------------------------------------------------------------
	// Pass A: build (or reuse the cached) 3D grading LUT containing the full
	// engine color grading + AgX tone curve + output device encoding. This is
	// what the engine's AddCombineLUTPass would produce for the filmic
	// tonemapper; the engine skips it entirely when a ReplacingTonemapper
	// delegate exists (PostProcessing.cpp ~line 1085), so we build our own.
	// ------------------------------------------------------------------
	FTextureRHIRef AgXContrastLUTRHI = GetOrCreateAgXContrastLUT_RenderThread();
	FRDGTextureRef ColorGradingLUT = AddTonemapMasterCombineLUTPass(
		GraphBuilder, View, GradingLUTCache, AgXContrastLUTRHI, LookMode, ContrastMode);

	// ------------------------------------------------------------------
	// Pass B: per-pixel exposure + bloom + vignette + LUT sample.
	// ------------------------------------------------------------------
	const FScreenPassTextureViewport SceneColorViewport(SceneColor);
	const FScreenPassTextureViewport BloomViewport(Bloom);
	const FScreenPassTextureViewport OutputViewport(Output);

	const FPostProcessSettings& PostProcessSettings = View.FinalPostProcessSettings;

	const FTonemapMasterOutputDeviceParameters OutputDeviceParams = GetTonemapMasterOutputDeviceParameters(*View.Family);

	// LUT addressing parameters (engine: PostProcessTonemap.cpp ~850, ~942-946).
	const float LUTSize = (float)ColorGradingLUT->Desc.Extent.Y;
	const float LUTMax = GetTonemapMasterLUTMax(OutputDeviceParams);

	FTonemapMasterPS::FParameters* Parameters = GraphBuilder.AllocParameters<FTonemapMasterPS::FParameters>();
	Parameters->View = View.ViewUniformBuffer;
	Parameters->Color = GetScreenPassTextureViewportParameters(SceneColorViewport);
	Parameters->Output = GetScreenPassTextureViewportParameters(OutputViewport);
	Parameters->Bloom = GetScreenPassTextureViewportParameters(BloomViewport);
	Parameters->ColorTexture = SceneColor.Texture;
	Parameters->ColorSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->BloomTexture = Bloom.Texture;
	Parameters->BloomSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->EyeAdaptationBuffer = GraphBuilder.CreateSRV(EyeAdaptationBuffer);
	// ChangeTextureUVCoordinateFromTo is not RENDERER_API in 5.8; compose its inline equivalent.
	Parameters->ColorToBloom =
		FScreenTransform::ChangeTextureBasisFromTo(SceneColorViewport, FScreenTransform::ETextureBasis::TextureUV, FScreenTransform::ETextureBasis::ViewportUV) *
		FScreenTransform::ChangeTextureBasisFromTo(BloomViewport, FScreenTransform::ETextureBasis::ViewportUV, FScreenTransform::ETextureBasis::TextureUV);
	Parameters->ColorScale0 = FVector4f(PostProcessSettings.SceneColorTint);

	// Vignette (engine: PostProcessTonemap.cpp ~889-908).
	Parameters->VignetteType = (uint32)PostProcessSettings.VignetteType;
	Parameters->VignetteParams0 = FVector4f(PostProcessSettings.VignetteIntensity, PostProcessSettings.VignetteColor.R, PostProcessSettings.VignetteColor.G, PostProcessSettings.VignetteColor.B);
	Parameters->VignetteParams1 = FVector4f(
		PostProcessSettings.VignetteCenter.X,
		PostProcessSettings.VignetteCenter.Y,
		1.0f / FMath::Max(0.001, PostProcessSettings.VignetteSize.X),
		1.0f / FMath::Max(0.001, PostProcessSettings.VignetteSize.Y));
	Parameters->VignetteParams2 = PostProcessSettings.VignetteSoftness;
	Parameters->VignetteTexture = GSystemTextures.GetWhiteDummy(GraphBuilder);
	Parameters->VignetteTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	if (PostProcessSettings.VignetteType == EVignetteType::Texture && PostProcessSettings.VignetteTexture)
	{
		FTextureResource* VignetteTextureResource = PostProcessSettings.VignetteTexture->GetResource();

		if (VignetteTextureResource && VignetteTextureResource->TextureRHI)
		{
			FRDGTextureRef VignetteTextureRDG = RegisterExternalTexture(GraphBuilder, VignetteTextureResource->TextureRHI, TEXT("TonemapMaster.VignetteTexture"));
			Parameters->VignetteTexture = VignetteTextureRDG;
		}
	}

	// Lens viewport mapping (engine: PostProcessTonemap.cpp ~947, ~1014-1017).
	Parameters->LensPrincipalPointOffsetScale = View.LensPrincipalPointOffsetScale;
	Parameters->LensPrincipalPointOffsetScaleInverse.X = -View.LensPrincipalPointOffsetScale.X / View.LensPrincipalPointOffsetScale.Z;
	Parameters->LensPrincipalPointOffsetScaleInverse.Y = -View.LensPrincipalPointOffsetScale.Y / View.LensPrincipalPointOffsetScale.W;
	Parameters->LensPrincipalPointOffsetScaleInverse.Z = 1.0f / View.LensPrincipalPointOffsetScale.Z;
	Parameters->LensPrincipalPointOffsetScaleInverse.W = 1.0f / View.LensPrincipalPointOffsetScale.W;

	Parameters->ColorGradingLUT = ColorGradingLUT;
	Parameters->ColorGradingLUTSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->LUTShaper = OutputDeviceParams.LUTShaper;
	Parameters->LUTMax = LUTMax;
	Parameters->LUTScale = (LUTSize - 1.0f) / LUTSize;
	Parameters->LUTOffset = 0.5f / LUTSize;

	// Backbuffer quantization dithering (engine: PostProcessTonemap.cpp ~916-941;
	// SDR-only so the HDR opt-out case is skipped).
	{
		Parameters->BackbufferQuantizationDithering = 1.0f / 1023.0f;

		if (View.Family->RenderTarget && View.Family->RenderTarget->GetRenderTargetTexture())
		{
			EPixelFormat BackbufferPixelFormat = View.Family->RenderTarget->GetRenderTargetTexture()->GetFormat();
			if (BackbufferPixelFormat == PF_B8G8R8A8 || BackbufferPixelFormat == PF_R8G8B8A8)
			{
				Parameters->BackbufferQuantizationDithering = 1.0f / 255.0f;
			}
		}

		GrainRandomFromFrame(&Parameters->GrainRandomFull, View.Family->FrameNumber);
	}
	Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();

	// Vignette permutations (engine: BuildCommonPermutationDomain ~157-158).
	FTonemapMasterPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FTonemapMasterPS::FAdvancedVignetteDim>(PostProcessSettings.VignetteType != EVignetteType::CosineFourthLaw);
	PermutationVector.Set<FTonemapMasterPS::FVignetteTextureDim>(PostProcessSettings.VignetteType == EVignetteType::Texture);

	TShaderMapRef<FTonemapMasterPS> PixelShader(GetGlobalShaderMap(View.GetFeatureLevel()), PermutationVector);

	AddDrawScreenPass(
		GraphBuilder,
		RDG_EVENT_NAME("TonemapMaster.AgX LUT (%dx%d)", OutputViewport.Rect.Width(), OutputViewport.Rect.Height()),
		View,
		OutputViewport,
		SceneColorViewport,
		PixelShader,
		Parameters);

	return MoveTemp(Output);
}
