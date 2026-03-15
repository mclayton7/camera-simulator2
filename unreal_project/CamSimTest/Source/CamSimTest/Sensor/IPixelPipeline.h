// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Config/CamSimConfig.h"   // FCamSimConfig::FPhase18Config, FHudOverlayConfig
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
	                        const TMap<ESensorMode, FSensorModeConfig>& Configs,
	                        const FSensorQualityConfig& QualityConfig) = 0;

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

	// Optional mutable settings — no-op defaults allow callers to use the
	// interface directly without dynamic_cast to FSensorPostProcess.
	virtual void SetGpuSensorEffectsActive(bool /*bActive*/) {}
	virtual void SetPhase18Config(const FCamSimConfig::FPhase18Config& /*Cfg*/) {}
	virtual void SetOverlayConfig(const FHudOverlayConfig& /*Cfg*/) {}
	virtual void SetLaserDesignatorConfig(const FCamSimConfig::FLaserDesignatorConfig& /*Cfg*/) {}
};
