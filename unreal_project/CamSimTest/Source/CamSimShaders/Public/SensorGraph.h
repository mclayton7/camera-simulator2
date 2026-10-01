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
	FIntRect       BloomViewRect;              // sampled at the scene sample's normalised position, clamped to this rect
	FRHIUniformBuffer* ViewUniformBuffer = nullptr;  // runtime: divides out View.OneOverPreExposure; null in tests (InputScale used)
	FIntPoint      OutputSize = FIntPoint::ZeroValue;  // capture size; X % 4 == 0, Y % 2 == 0
	/** ROADMAP 4A: SceneColor is in-band radiance (runtime: the RGBA16F scene colour holding (L, L, L, 1) that ThermalCS
	 *  wrote at BeforeDOF and TSR resolved; tests may pass ThermalCS's R32F (L, 0, 0)). SignalWeights (1, 0, 0) read L.
	 *  View pre-exposure is not divided out (InputScale = 1 / B(300 K) scales it) and Bloom is ignored. */
	bool           bRadianceInput = false;
};

struct FSensorGraphOutputs
{
	FRDGTextureRef SensorRgb = nullptr;   // OutputSize, PF_FloatRGBA: display RGB, .a = luma source
	FRDGBufferRef  Nv12      = nullptr;   // OutputSize.X*Y*3/2 bytes as uint32 structured buffer
	FRDGBufferRef  Histogram = nullptr;   // 256 x uint32 structured buffer
	uint32 Nv12Bytes = 0;
};

/**
 * Whether this RHI can run the sensor graph: a real RHI (not NullRHI), SM5
 * compute, and the sensor compute shader (every permutation) in the global shader
 * map. Game thread, after RHI init. On false, OutWhy says what's missing.
 */
CAMSIMSHADERS_API bool IsSensorGraphSupported(FString& OutWhy);

/**
 * Adds the sensor graph to GraphBuilder: one fused compute pass, SensorCS (sanitize, scale, [+bloom],
 * distortion resample, cos^n, histogram, PSF blur from Params.PsfTaps (skipped for a single tap),
 * detector, ADC, defects, display, BT.709 limited-range NV12).
 * Order of operations and every sampling convention follow CamSimSensorRef::Run
 * (Sensor/SensorReference.h) exactly; CamSim.GPU.Sensor.* compare the two.
 */
CAMSIMSHADERS_API FSensorGraphOutputs AddSensorPasses(FRDGBuilder& GraphBuilder, const FSensorGraphInputs& In,
	const FSensorFrameParams& Params);
