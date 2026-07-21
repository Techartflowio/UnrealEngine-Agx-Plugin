// Copyright (c) 2026. TonemapMaster plugin.

#pragma once

#include "SceneViewExtension.h"

#include "TonemapMasterCombineLUTs.h"

class FTonemapMasterSceneViewExtension : public FSceneViewExtensionBase
{
public:
	FTonemapMasterSceneViewExtension(const FAutoRegister& AutoRegister);

	//~ ISceneViewExtension interface
	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView, FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	FScreenPassTexture ReplaceTonemapper_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs);

	// Lazily creates the RHI texture for the AgX_Default_Contrast LUT on first
	// use, then returns the cached texture. Called from the render thread.
	FTextureRHIRef GetOrCreateAgXContrastLUT_RenderThread();

	// Lazily created 1x4096 R32_FLOAT texture holding the AgX_Default_Contrast
	// curve. Stored on the extension so we only upload it once. Consumed by the
	// Pass A grading-LUT builder (TonemapMasterCombineLUTs).
	FTextureRHIRef AgXContrastLUT;

	// Persistent 3D grading LUT (engine color grading + AgX + output device
	// encoding) built by Pass A and sampled per-pixel by Pass B. Rebuilt only
	// when the hashed settings change.
	FTonemapMasterGradingLUTCache GradingLUTCache;
};
