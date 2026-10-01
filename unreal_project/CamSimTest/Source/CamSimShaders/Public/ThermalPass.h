// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphDefinitions.h"
#include "ThermalFrameParams.h"

class FRHIUniformBuffer;

namespace CamSimThermalPass
{
	/**
	 * ROADMAP 4A Task 1 spike, recorded in ROADMAP.md ("4A Thermal core"): GBufferC holds the material base colour at
	 * ReplacingTonemapper with r.Substrate=True and r.Substrate.ProjectGBufferFormat=0. false = FALLBACK: the fast solar
	 * term is off (FThermalFrameParams::KFastScale = 0) and UCamSimSubsystem logs one warning.
	 * Record -> values: LINEAR -> true/false; SRGB_ENCODED -> true/true; FALLBACK -> false/false.
	 */
	inline constexpr bool bBaseColorAtTonemapper = true;
	/** Same spike: GBufferC values are sRGB-encoded in a non-sRGB format; ThermalCS decodes them (FThermalFrameParams::bBaseColorSrgb). */
	inline constexpr bool bBaseColorSrgbEncoded = false;
}

/** Inputs of ThermalCS (ROADMAP 4A). */
struct FThermalPassInputs
{
	FRDGTextureRef    SceneColor    = nullptr;   // tonemapper input (HDR; pre-exposed unless ViewUniformBuffer is null)
	FIntRect          SceneColorRect;            // its view rect; the radiance texture is SceneColorRect.Size()
	FRDGTextureRef    SceneDepth    = nullptr;   // device Z (reversed: 1 near, 0 far)
	FRDGTextureRef    CustomDepth   = nullptr;   // device Z of the tagged entities
	FRDGTextureSRVRef CustomStencil = nullptr;   // uint; read via STENCIL_COMPONENT_SWIZZLE
	FRDGTextureRef    BaseColor     = nullptr;   // GBufferC; null: fast term off
	FIntRect          DepthViewRect;             // region of the depth/stencil/base textures (ignored when ViewUniformBuffer is set)
	FRHIUniformBuffer* ViewUniformBuffer = nullptr;   // runtime: rect = View.ViewRectMin/ViewSizeAndInvSize, colour * View.OneOverPreExposure
	/**
	 * Task 17 (BeforeDOF): when set, ThermalCS writes float4(L, L, L, 1) into this texture (scene-colour format, typically
	 * PF_FloatRGBA; needs TexCreate_UAV) at OutputRect (size = SceneColorRect.Size()) instead of allocating its R32F
	 * output. Texels outside OutputRect are untouched. Must not be SceneColor itself (no read/write aliasing).
	 */
	FRDGTextureRef    OutputSceneColor = nullptr;
	FIntRect          OutputRect;
};

/** Whether ThermalCS (every permutation) is in the global shader map. Game thread, after RHI init. */
CAMSIMSHADERS_API bool IsThermalPassSupported(FString& OutWhy);

/**
 * ThermalCS: in-band radiance (W m^-2 sr^-1, raw — no pre-exposure). Without OutputSceneColor: a new R32F texture, extent
 * SceneColorRect.Size(), its view rect at (0, 0). With it: returns OutputSceneColor, written at OutputRect. See CamSimThermalRef.
 */
CAMSIMSHADERS_API FRDGTextureRef AddThermalPass(FRDGBuilder& GraphBuilder, const FThermalPassInputs& In, const FThermalFrameParams& Params);
