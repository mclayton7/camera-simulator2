// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphDefinitions.h"
#include "SensorFrameParams.h"

class FRHIUniformBuffer;

/** Inputs of InstanceIdCS (ground truth, ROADMAP 2.7). */
struct FInstanceIdInputs
{
	FRDGTextureRef    SceneDepth  = nullptr;   // device Z (reversed: 1 near, 0 far)
	FRDGTextureRef    CustomDepth = nullptr;   // device Z of the tagged entities only
	FRDGTextureSRVRef CustomStencil = nullptr; // uint; value replicated or read via STENCIL_COMPONENT_SWIZZLE
	FIntRect DepthViewRect;                    // region of the depth textures the view covers (ignored when ViewUniformBuffer is set)
	FRHIUniformBuffer* ViewUniformBuffer = nullptr;  // runtime: the rect comes from View.ViewRectMin / ViewSizeAndInvSize
	FIntPoint OutputSize = FIntPoint::ZeroValue;     // = sensor output, X even
};

/**
 * Whether InstanceIdCS (both view-rect permutations) is in the global shader map. Game thread, after RHI
 * init. Kept apart from IsSensorGraphSupported so a missing ground-truth shader cannot disable the video path.
 */
CAMSIMSHADERS_API bool IsInstanceIdPassSupported(FString& OutWhy);

/** One uint32 per two output pixels (pixel 2k low 16 bits): visible | amodal << 8. FocalPx/K1/K2 from Params. */
CAMSIMSHADERS_API FRDGBufferRef AddInstanceIdPass(FRDGBuilder& GraphBuilder, const FInstanceIdInputs& In, const FSensorFrameParams& Params);
