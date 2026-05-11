// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Metadata/KlvBuilder.h"   // FCamSimTelemetry
#include "Sensor/SensorTypes.h"    // ESensorMode, FSensorModeConfig
#include "Sensor/IPixelPipeline.h" // IPixelPipeline
#include "Config/CamSimConfig.h"   // FCamSimConfig::FPhase18Config
#include "Overlay/FHudOverlay.h"   // FHudOverlay, FHudOverlayConfig

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
	                        const TMap<ESensorMode, FSensorModeConfig>& InConfigs,
	                        const FSensorQualityConfig& InQualityConfig) override;

	/**
	 * Configure Brown-Conrady radial lens distortion remap table.
	 * K1=0, K2=0 produces identity (no distortion).
	 * Must be called after Initialize().
	 */
	void SetDistortion(float K1, float K2);

	/**
	 * Apply Phase 18 config (precipitation, dynamic IR extinction).
	 * Safe to call from game thread before each frame is submitted.
	 */
	virtual void SetPhase18Config(const FCamSimConfig::FPhase18Config& Cfg) override;

	/**
	 * Apply Phase 20 HUD/OSD overlay config.
	 * Safe to call from game thread before each frame is submitted.
	 */
	virtual void SetOverlayConfig(const FHudOverlayConfig& Cfg) override { HudOverlay.SetConfig(Cfg); }

	/** Phase 21F.1 — Laser designator spot config. */
	virtual void SetLaserDesignatorConfig(const FCamSimConfig::FLaserDesignatorConfig& Cfg) override
	{
		LaserDesignatorConfig = Cfg;
	}

	/** Phase 27A — GPU sensor pipeline: skip CPU waveband/AGC/noise loops when GPU material is active. */
	virtual void SetGpuSensorEffectsActive(bool bActive) override { bGpuSensorEffectsActive_ = bActive; }

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
	FSensorQualityConfig Quality;

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

	/** Scene-wide attenuation toward mid-gray based on visibility distance. */
	void ApplyAtmosphericAttenuation(TArray<FColor>& Pixels, float VisibilityM, float SlantRangeM, float Strength);

	/** Adjust white balance toward a target color temperature (Kelvin). */
	void ApplyColorTemperature(TArray<FColor>& Pixels, float Kelvin);

	/** Apply global contrast and brightness adjustment. */
	void ApplyContrastBrightness(TArray<FColor>& Pixels, float Contrast, float BrightnessBias);

	/** Apply a separable box blur with clamped edges. */
	void ApplyBoxBlur(TArray<FColor>& Pixels, int32 Radius);

	// -- Lens distortion (Phase 15B) -----------------------------------------

	/** Precomputed remap table [W*H*2]: (srcX, srcY) per output pixel. */
	TArray<float> DistortionRemap;
	bool bDistortionEnabled = false;

	/** Apply Brown-Conrady radial lens distortion using precomputed remap. */
	void ApplyLensDistortion(TArray<FColor>& Pixels);

	// -- Phase 16: Sensor Fidelity -------------------------------------------

	/** Pre-allocated 256-bin histogram scratch buffer (avoids per-frame alloc). */
	TArray<int32> AGCHistogram;

	// -- Per-frame scratch buffers (Phase 2: avoid hot-path heap allocs) ----
	// Sized once in Initialize(W, H). Each Apply* function that needed a local
	// TArray<FColor> now uses one of these. They are NOT concurrently used — the
	// Apply* pipeline is strictly serial on the encoder task thread.
	//   - BlurTemp_      : separable-blur intermediate (ApplyBoxBlur, ApplyGaussianBlur)
	//   - ScratchFrameA_ : full-frame source snapshot (ApplyLensDistortion, ApplyVibration)
	//   - ScratchFrameB_ : reserved for a second snapshot if a future effect needs one
	//                      (ApplyRollingShutter uses it for the unblended snapshot)
	TArray<FColor> BlurTemp_;
	TArray<FColor> ScratchFrameA_;
	TArray<FColor> ScratchFrameB_;

	/** Pre-baked 4x4 Bayer ordered dither matrix [0, 15]. */
	uint8 BayerMatrix[16] = {};

	/** Sparse defect pixel indices into [W*H]. Built once in Initialize(). */
	TArray<int32> DefectIndices;
	/** Corresponding defect values: 255 = hot, 0 = dead. */
	TArray<uint8> DefectValues;

	/** 16A: Histogram percentile stretch (AGC). Computed live per frame. */
	void ApplyRadianceAGC(TArray<FColor>& Pixels, const FSensorModeConfig& Cfg);

	/** 16A: Manual level/gain override when AGC is disabled. */
	void ApplyManualGain(TArray<FColor>& Pixels, float Level, float Gain);

	/** 16B: A/D quantization to N bits with optional ordered dither. */
	void ApplyQuantization(TArray<FColor>& Pixels, int32 Bits, bool bDither);

	/** 16C: Stamp pre-baked hot/dead pixel defects onto the image. */
	void ApplyDefectPixels(TArray<FColor>& Pixels);

	/** 16D: Separable Gaussian PSF blur (replaces box blur when sigma > 0). */
	void ApplyGaussianBlur(TArray<FColor>& Pixels, float Sigma);

	/** 16F: Sinusoidal horizontal AC banding artifact (IR readout simulation). */
	void ApplyACBanding(TArray<FColor>& Pixels, float Frequency, float Amplitude, uint64 FrameIndex);

	/** 16L: Gaussian bright spot for NVG IR pointer overlay. */
	void ApplyIRPointer(TArray<FColor>& Pixels, const FSensorModeConfig& Cfg);

	// -- Phase 16 Sprint 2: Sensor Fidelity -----------------------------------

	// 16E: Thermal drift state
	float DriftAccumulatorDN = 0.0f;   // current baseline shift in DN
	float DriftElapsedSec    = 0.0f;   // time since last NUC reset

	// 16G: AGC lag smoothed thresholds
	float AGCSmoothedLo = -1.0f;       // -1 = not yet initialized
	float AGCSmoothedHi = -1.0f;

	// 16H: Previous frame for rolling shutter blending
	TArray<FColor> PreviousFrame;

	/** 16E: Gradual IR baseline drift with periodic NUC reset. */
	void ApplyThermalDrift(TArray<FColor>& Pixels, const FSensorModeConfig& Cfg, float DeltaTime);

	/** 16H: Per-row blend between previous and current frame. */
	void ApplyRollingShutter(TArray<FColor>& Pixels, float Strength);

	/** 16I: Subpixel random displacement (platform microdynamics). */
	void ApplyVibration(TArray<FColor>& Pixels, float Amplitude, uint64 FrameIndex);

	/** 16J: Per-frame global gain and offset jitter. */
	void ApplyGainOffsetJitter(TArray<FColor>& Pixels, float GainJitter, float OffsetJitter, uint64 FrameIndex);

	/** 16K: Brighten highlight pixels based on sun angle (EO sun glint). */
	void ApplySunGlint(TArray<FColor>& Pixels, const FSensorModeConfig& Cfg, float SunElevationDeg);

	// -- Fused pixel pipeline (Phase 28 optimization) -----------------------

	/** Fused per-pixel pass: combines waveband remap, AGC, tone controls,
	 *  noise, vignetting, scan lines, and atmospheric effects into a single
	 *  cache-friendly iteration. Returns true if fusion was applied. */
	bool ProcessFusedPerPixel(TArray<FColor>& Pixels, ESensorMode Mode, uint8 Polarity,
	                          const FSensorModeConfig& Cfg, const FCamSimTelemetry& Telemetry,
	                          uint64 FrameIndex);

	// -- Phase 27A: GPU sensor pipeline bypass flag --------------------------

	bool bGpuSensorEffectsActive_ = false;

	// -- Phase 18: Weather, Atmosphere & Particle Effects --------------------

	FCamSimConfig::FPhase18Config Phase18;

	// -- Phase 20: HUD/OSD overlay -------------------------------------------

	FHudOverlay HudOverlay;

	// -- Phase 21F.1: Laser designator visualization -------------------------

	FCamSimConfig::FLaserDesignatorConfig LaserDesignatorConfig;

	/** Gaussian laser spot overlay — renders per-mode (EO/IR/NVG). */
	void ApplyLaserDesignator(TArray<FColor>& Pixels, ESensorMode Mode, uint8 Polarity);

	/**
	 * 18D: CPU precipitation overlay (rain streaks / snow flakes).
	 * Uses per-frame FRandomStream seeded from FrameIndex for thread safety.
	 */
	void ApplyPrecipitation(TArray<FColor>& Pixels, uint64 FrameIndex);

	/**
	 * 18K: Override IR extinction coefficient from atmospheric visibility.
	 * Maps VisibilityRangeM to an extinction coefficient, then calls ApplyIRExtinction.
	 */
	void ApplyDynamicIRExtinction(TArray<FColor>& Pixels, float SlantRangeM);
};
