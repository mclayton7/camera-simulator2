// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Metadata/KlvBuilder.h"  // FCamSimTelemetry
#include "Sensor/SensorTypes.h"   // ESensorMode, FSensorModeConfig

/**
 * IPixelPipeline
 *
 * Pure-virtual interface for CPU-side per-frame pixel post-processing.
 * FSensorPostProcess implements this for EO / IR / NVG waveband simulation.
 *
 * Enables test injection of a no-op or diagnostic pipeline without
 * linking against the full sensor implementation.
 */
class IPixelPipeline
{
public:
	virtual ~IPixelPipeline() = default;

	/**
	 * Pre-compute lookup tables and noise buffers.
	 * Must be called once before any Process() calls.
	 */
	virtual void Initialize(int32 Width, int32 Height,
	                        const TMap<ESensorMode, FSensorModeConfig>& Configs) = 0;

	/**
	 * Apply the sensor pipeline in-place.
	 * Called from the async encode task thread; safe because each frame
	 * owns its TArray<FColor> exclusively.
	 */
	virtual void Process(TArray<FColor>& Pixels,
	                     ESensorMode     Mode,
	                     uint8           Polarity,
	                     const FCamSimTelemetry& Telemetry,
	                     uint64          FrameIndex) = 0;
};
