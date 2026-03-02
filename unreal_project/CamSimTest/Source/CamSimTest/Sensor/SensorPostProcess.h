// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Metadata/KlvBuilder.h"   // FCamSimTelemetry
#include "Sensor/SensorTypes.h"    // ESensorMode, FSensorModeConfig
#include "Sensor/IPixelPipeline.h" // IPixelPipeline

// ---------------------------------------------------------------------------
// FSensorPostProcess — CPU-side pixel pipeline applied after GPU readback
// ---------------------------------------------------------------------------
class FSensorPostProcess : public IPixelPipeline
{
public:
	FSensorPostProcess() = default;
	virtual ~FSensorPostProcess() override = default;

	// IPixelPipeline interface
	virtual void Initialize(int32 InWidth, int32 InHeight,
	                        const TMap<ESensorMode, FSensorModeConfig>& InConfigs) override;

	/**
	 * Apply the sensor pipeline in-place.
	 * Called from the async task thread — safe to modify Pixels without a lock
	 * because each frame owns its TArray exclusively.
	 *
	 * @param Pixels       BGRA8 pixel buffer (W × H entries).
	 * @param Mode         Active waveband.
	 * @param Polarity     0 = WhiteHot, 1 = BlackHot (IR only).
	 * @param Telemetry    Current platform telemetry (SlantRangeM used for IR extinction).
	 * @param FrameIndex   Monotonically increasing frame counter (drives noise ring).
	 */
	virtual void Process(TArray<FColor>& Pixels,
	                     ESensorMode     Mode,
	                     uint8           Polarity,
	                     const FCamSimTelemetry& Telemetry,
	                     uint64          FrameIndex) override;

private:
	int32  Width  = 0;
	int32  Height = 0;
	TMap<ESensorMode, FSensorModeConfig> Configs;

	/** Precomputed radial vignetting weights [W*H], range [0, 1]. */
	TArray<float>  VignetteWeights;

	/** Static per-pixel noise offsets [W*H], range [-128, 127]. */
	TArray<int16>  FixedPatternMap;

	/**
	 * Pre-baked Gaussian noise ring [2*W*H].
	 * Indexed by (pixel_idx + (FrameIndex%2) * W*H) % ring_size.
	 * Values scaled so that 1.0 NETD stdev ≈ 255 DN.
	 */
	TArray<int16>  NoiseRing;

	/** IR detector response: mild S-curve, luminance → thermal intensity. */
	uint8 IRToneCurve[256]   = {};

	/** NVG image-intensifier response: gamma 0.45 lift, luminance → green intensity. */
	uint8 NVGGammaCurve[256] = {};

	// -- Effect implementations ----------------------------------------------

	/** BT.601 luma → grayscale → tone curve → optional polarity inversion. */
	void ApplyIR(TArray<FColor>& Pixels, uint8 Polarity);

	/** BT.601 luma → gamma lift → P22 green phosphor tint. */
	void ApplyNVG(TArray<FColor>& Pixels);

	/** Koschmieder-style scene-wide IR haze using SlantRangeM as depth proxy. */
	void ApplyIRExtinction(TArray<FColor>& Pixels, float Coeff, float SlantRangeM);

	/** Add per-frame Gaussian noise from pre-baked ring buffer. */
	void ApplyNoise(TArray<FColor>& Pixels, float NETD, uint64 FrameIndex);

	/** Add static per-pixel bias from FixedPatternMap. */
	void ApplyFixedPatternNoise(TArray<FColor>& Pixels, float FPNAmplitude);

	/** Multiply each pixel by a precomputed radial weight. */
	void ApplyVignetting(TArray<FColor>& Pixels, float Strength);

	/** Alternating row brightness reduction. */
	void ApplyScanLines(TArray<FColor>& Pixels, float Strength);
};
