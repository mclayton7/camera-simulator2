// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphDefinitions.h"
#include "SensorFrameParams.h"

class FRHIUniformBuffer;

/** Inputs of the GPU sensor graph (ROADMAP 3B). */
struct FSensorGraphInputs
{
	FRDGTextureRef SceneColor = nullptr;       // HDR, pre-exposed unless View is null
	FIntRect       SceneViewRect;              // region of SceneColor to sample
	FRDGTextureRef Bloom = nullptr;            // optional (UE CombinedBloom), same units as SceneColor
	FIntRect       BloomViewRect;
	FRHIUniformBuffer* ViewUniformBuffer = nullptr;  // runtime: divides out View.OneOverPreExposure; null in tests (InputScale used)
	FIntPoint      OutputSize = FIntPoint::ZeroValue;  // capture size; X % 4 == 0, Y % 2 == 0
};

struct FSensorGraphOutputs
{
	FRDGTextureRef SensorRgb = nullptr;   // OutputSize, PF_FloatRGBA: display RGB (tinted), .a = luma source
	FRDGBufferRef  Nv12      = nullptr;   // OutputSize.X*Y*3/2 bytes as uint32 structured buffer
	FRDGBufferRef  Histogram = nullptr;   // 256 x uint32 structured buffer
	uint32 Nv12Bytes = 0;
};

/**
 * Adds the sensor passes (ApplyCS: sanitize, gain, histogram, EO/IR/NVG transfer;
 * PackNv12CS: BT.709 limited-range NV12) to GraphBuilder. Order of operations
 * follows CamSimSensorRef (Sensor/SensorReference.h) exactly.
 */
/**
 * Whether this RHI can run the sensor graph: a real RHI (not NullRHI), SM5
 * compute, and both compute shaders (every permutation) in the global shader
 * map. Game thread, after RHI init. On false, OutWhy says what's missing.
 */
CAMSIMSHADERS_API bool IsSensorGraphSupported(FString& OutWhy);

CAMSIMSHADERS_API FSensorGraphOutputs AddSensorPasses(FRDGBuilder& GraphBuilder, const FSensorGraphInputs& In,
	const FSensorFrameParams& Params);
