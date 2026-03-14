// Copyright CamSim Contributors. All Rights Reserved.

#include "Sensor/SensorPostProcess.h"
#include "CamSimTest.h"
#include "Math/UnrealMathUtility.h"
#include "Async/ParallelFor.h"

// Number of horizontal bands used by ParallelFor (8 × 135 rows at 1080p).
// Each band is a disjoint row range — safe to process in parallel.
static constexpr int32 kParallelBands = 8;

// ---------------------------------------------------------------------------
// Initialize — bake LUTs and pre-generate noise buffers
// ---------------------------------------------------------------------------

void FSensorPostProcess::Initialize(int32 InWidth, int32 InHeight,
                                    const TMap<ESensorMode, FSensorModeConfig>& InConfigs,
                                    const FSensorQualityConfig& InQualityConfig)
{
	Width   = InWidth;
	Height  = InHeight;
	Configs = InConfigs;
	Quality = InQualityConfig;

	const int32 NumPixels  = Width * Height;
	const int32 RingSize   = 2 * NumPixels;

	// -------------------------------------------------------------------------
	// IR tone curve: mild S-curve  t < 0.5 → 2t²,  t >= 0.5 → 1 - 2(1-t)²
	// Simulates FPA detector non-linearity (boosted contrast in mid-range).
	// -------------------------------------------------------------------------
	for (int32 L = 0; L < 256; ++L)
	{
		const float t = L / 255.0f;
		float curve;
		if (t < 0.5f)
			curve = 2.0f * t * t;
		else
			curve = 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
		IRToneCurve[L] = static_cast<uint8>(FMath::RoundToInt(curve * 255.0f));
	}

	// -------------------------------------------------------------------------
	// NVG gamma curve: gamma 0.45 lift (image intensifier low-light boost)
	// -------------------------------------------------------------------------
	for (int32 L = 0; L < 256; ++L)
	{
		const float t = L / 255.0f;
		NVGGammaCurve[L] = static_cast<uint8>(FMath::RoundToInt(FMath::Pow(t, 0.45f) * 255.0f));
	}

	// -------------------------------------------------------------------------
	// Vignetting weight table: radial cubic falloff, raw (Strength applied at runtime)
	// VignetteWeights[i] = 1 - r²*sqrt(r²)  where r is normalised distance [-1,1]
	// -------------------------------------------------------------------------
	VignetteWeights.SetNumUninitialized(NumPixels);
	const float HalfW = Width  * 0.5f;
	const float HalfH = Height * 0.5f;
	for (int32 Y = 0; Y < Height; ++Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			const float dx = (X - HalfW) / HalfW;
			const float dy = (Y - HalfH) / HalfH;
			const float r2 = dx * dx + dy * dy;
			// Raw radial weight: 1 - r²·|r| (cubic roll-off)
			VignetteWeights[Y * Width + X] = FMath::Max(0.0f, 1.0f - r2 * FMath::Sqrt(r2));
		}
	}

	// -------------------------------------------------------------------------
	// Fixed pattern noise map: static per-pixel bias in [-128, 127]
	// Seeded with a fixed value for reproducibility.
	// -------------------------------------------------------------------------
	FixedPatternMap.SetNumUninitialized(NumPixels);
	FMath::SRandInit(12345);
	for (int32 i = 0; i < NumPixels; ++i)
	{
		// SRand returns [0, 1); map to [-1, 1)
		const float v = FMath::SRand() * 2.0f - 1.0f;
		FixedPatternMap[i] = static_cast<int16>(FMath::RoundToInt(v * 127.0f));
	}

	// -------------------------------------------------------------------------
	// NETD noise ring: 2 frames of Gaussian samples scaled to DN
	// Each entry represents ±(NETD * 255) before NETD scaling at runtime.
	// We store unit-scale values (stdev ≈ 1.0 in fraction terms, scaled ×255),
	// then multiply by the per-mode NETD coefficient in ApplyNoise().
	// Approx Gaussian: average 4 uniform samples for central limit theorem.
	// -------------------------------------------------------------------------
	NoiseRing.SetNumUninitialized(RingSize);
	FMath::SRandInit(67890);
	for (int32 i = 0; i < RingSize; ++i)
	{
		// 4-sample CLT approximation of a Gaussian with stdev ≈ 0.5
		float sum = (FMath::SRand() + FMath::SRand() + FMath::SRand() + FMath::SRand()) - 2.0f;
		// sum is in [-2, 2] with stdev ≈ 1.0 / sqrt(4*1/12) × ... ≈ 0.816
		// Normalize to unit stdev: sum / 0.816 × 255 → scale factor ≈ 312
		// We use 255 as the max excursion for NETD=1.0 (full range)
		NoiseRing[i] = static_cast<int16>(FMath::Clamp(FMath::RoundToInt(sum * 127.5f), -255, 255));
	}

	// -------------------------------------------------------------------------
	// 16A: AGC histogram scratch buffer (pre-allocated, zeroed per frame)
	// -------------------------------------------------------------------------
	AGCHistogram.SetNumZeroed(256);

	// -------------------------------------------------------------------------
	// 16B: Bayer ordered dither matrix (4x4)
	// Values in [0, 15] — thresholded against quantization step.
	// -------------------------------------------------------------------------
	static constexpr uint8 kBayerRaw[16] = {
		 0,  8,  2, 10,
		12,  4, 14,  6,
		 3, 11,  1,  9,
		15,  7, 13,  5
	};
	for (int32 i = 0; i < 16; ++i)
		BayerMatrix[i] = kBayerRaw[i];

	// -------------------------------------------------------------------------
	// 16C: Defect pixel map — sparse list of hot/dead pixel positions
	// Uses the first mode with DefectPixelCount > 0 as the source.
	// Seeded deterministically for reproducibility.
	// -------------------------------------------------------------------------
	int32 MaxDefects = 0;
	float HotRatio   = 0.5f;
	int32 DefSeed    = 42;
	for (const auto& Pair : Configs)
	{
		if (Pair.Value.DefectPixelCount > MaxDefects)
		{
			MaxDefects = Pair.Value.DefectPixelCount;
			HotRatio   = Pair.Value.DefectHotRatio;
			DefSeed    = Pair.Value.DefectSeed;
		}
	}
	if (MaxDefects > 0 && NumPixels > 0)
	{
		const int32 Count = FMath::Min(MaxDefects, NumPixels);
		DefectIndices.SetNumUninitialized(Count);
		DefectValues.SetNumUninitialized(Count);

		// Rejection sampling for unique indices
		TSet<int32> UsedIndices;
		UsedIndices.Reserve(Count);
		FMath::SRandInit(DefSeed);
		int32 Placed = 0;
		for (int32 i = 0; i < Count; ++i)
		{
			int32 Idx = -1;
			constexpr int32 MaxAttempts = 1000;
			for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
			{
				Idx = static_cast<int32>(FMath::SRand() * NumPixels) % NumPixels;
				if (!UsedIndices.Contains(Idx)) break;
				if (Attempt == MaxAttempts - 1) Idx = -1;
			}
			if (Idx < 0)
			{
				UE_LOG(LogCamSim, Warning, TEXT("FSensorPostProcess: defect pixel placement exhausted after %d placed"), Placed);
				break;
			}
			UsedIndices.Add(Idx);
			DefectIndices[Placed] = Idx;
			DefectValues[Placed] = (FMath::SRand() < HotRatio) ? 255u : 0u;
			++Placed;
		}
		if (Placed < Count)
		{
			DefectIndices.SetNum(Placed);
			DefectValues.SetNum(Placed);
		}
	}

	UE_LOG(LogCamSim, Log,
		TEXT("FSensorPostProcess: initialized %dx%d ring=%d EO/IR/NVG=%d/%d/%d defects=%d quality(noise=%.2f vignette=%.2f scan=%.2f atmos=%.2f blur=%d contrast=%.2f bias=%.2f)"),
		Width, Height, RingSize,
		Configs.Contains(ESensorMode::EO) ? 1 : 0,
		Configs.Contains(ESensorMode::IR) ? 1 : 0,
		Configs.Contains(ESensorMode::NVG) ? 1 : 0,
		DefectIndices.Num(),
		Quality.NoiseScale, Quality.VignettingScale, Quality.ScanLineScale,
		Quality.AtmosphereScale, Quality.BlurRadius, Quality.Contrast, Quality.BrightnessBias);
}

// ---------------------------------------------------------------------------
// Process — orchestrate the effect pipeline for one frame
// ---------------------------------------------------------------------------

void FSensorPostProcess::Process(TArray<FColor>& Pixels,
                                 ESensorMode     Mode,
                                 uint8           Polarity,
                                 const FCamSimTelemetry& Telemetry,
                                 uint64          FrameIndex)
{
	if (Pixels.Num() != Width * Height)
	{
		UE_LOG(LogCamSim, Warning,
			TEXT("FSensorPostProcess::Process: pixel count mismatch (%d vs %d×%d)"),
			Pixels.Num(), Width, Height);
		return;
	}

	// 27A — GPU post-process material handles tone mapping and noise on GPU.
	// Only run cheap stateful CPU effects (defect pixels, quantization, overlay).
	if (bGpuSensorEffectsActive_)
	{
		const FSensorModeConfig* ModeCfg = Configs.Find(Mode);
		if (ModeCfg)
		{
			ApplyDefectPixels(Pixels);
			ApplyQuantization(Pixels, ModeCfg->QuantizationBits, ModeCfg->bQuantizationDither);
		}
		if (Phase18.bPrecipitation)
			ApplyPrecipitation(Pixels, FrameIndex);
		HudOverlay.Render(Pixels, Telemetry);
		return;
	}

	const FSensorModeConfig* CfgPtr = Configs.Find(Mode);
	if (!CfgPtr)
	{
		// No config for this mode — passthrough with no effects
		return;
	}
	const FSensorModeConfig& Cfg = *CfgPtr;

	// Step 1: Waveband remapping (must be first — converts to grayscale for IR/NVG)
	switch (Mode)
	{
		case ESensorMode::EO:
			// Color passthrough — no conversion needed
			break;
		case ESensorMode::IR:
			ApplyIR(Pixels, Polarity);
			break;
		case ESensorMode::NVG:
			ApplyNVG(Pixels);
			break;
	}

	// Step 1.1: 16J — Sensor gain/offset jitter (IR only, before AGC)
	if (Mode == ESensorMode::IR &&
		(Cfg.GainJitter > 0.0f || Cfg.OffsetJitter > 0.0f))
	{
		ApplyGainOffsetJitter(Pixels, Cfg.GainJitter, Cfg.OffsetJitter, FrameIndex);
	}

	// Step 1A: 16A — Radiance AGC or manual gain (IR/NVG only, after grayscale)
	if (Mode == ESensorMode::IR || Mode == ESensorMode::NVG)
	{
		const bool bManual = (Cfg.AGCManualLevel >= 0.0f);
		if (Cfg.bAGCEnabled && !bManual)
		{
			ApplyRadianceAGC(Pixels, Cfg);
		}
		else if (bManual)
		{
			ApplyManualGain(Pixels, Cfg.AGCManualLevel, Cfg.AGCManualGain);
		}
	}

	// Step 1B: 16B — Quantization (after AGC, before tone controls)
	if (Cfg.QuantizationBits > 0 && Cfg.QuantizationBits < 8)
	{
		ApplyQuantization(Pixels, Cfg.QuantizationBits, Cfg.bQuantizationDither);
	}

	// Step 1.5: 16E — Thermal drift (IR only, after AGC/quantization)
	if (Mode == ESensorMode::IR && Cfg.bThermalDriftEnabled)
	{
		// Assume 30fps (matches DefaultEngine.ini FixedFrameRate=30)
		ApplyThermalDrift(Pixels, Cfg, 1.0f / 30.0f);
	}

	// Step 1C: 16L — NVG IR pointer (NVG only, after waveband remap)
	if (Mode == ESensorMode::NVG && Cfg.bIRPointerEnabled)
	{
		ApplyIRPointer(Pixels, Cfg);
	}

	// Step 2: optional color-temperature shift
	if (Cfg.ColorTemperatureK > 1000.0f)
	{
		ApplyColorTemperature(Pixels, Cfg.ColorTemperatureK);
	}

	// Step 3: mode-level + quality-level tone controls
	const float EffectiveContrast = FMath::Clamp(Cfg.Contrast * Quality.Contrast, 0.1f, 4.0f);
	const float EffectiveBias = FMath::Clamp(Cfg.BrightnessBias + Quality.BrightnessBias, -1.0f, 1.0f);
	if (!FMath::IsNearlyEqual(EffectiveContrast, 1.0f, KINDA_SMALL_NUMBER) ||
		!FMath::IsNearlyZero(EffectiveBias, KINDA_SMALL_NUMBER))
	{
		ApplyContrastBrightness(Pixels, EffectiveContrast, EffectiveBias);
	}

	// Step 4: visibility-based attenuation for all wavebands
	if (Cfg.AtmosphericVisibilityM > 0.0f && Telemetry.SlantRangeM > 0.0)
	{
		const float Strength = FMath::Clamp(Cfg.AtmosphereStrength * Quality.AtmosphereScale, 0.0f, 2.0f);
		if (Strength > 0.0f)
		{
			ApplyAtmosphericAttenuation(Pixels, Cfg.AtmosphericVisibilityM,
				static_cast<float>(Telemetry.SlantRangeM), Strength);
		}
	}

	// Step 5: legacy IR extinction model (kept for backwards compatibility)
	if (Mode == ESensorMode::IR && Cfg.IRExtinctionCoeff > 0.0f && Telemetry.SlantRangeM > 0.0)
	{
		ApplyIRExtinction(Pixels, Cfg.IRExtinctionCoeff * Quality.AtmosphereScale,
			static_cast<float>(Telemetry.SlantRangeM));
	}

	// Step 6: Temporal NETD noise (from pre-baked ring buffer)
	const float EffectiveNETD = Cfg.NETD * Quality.NoiseScale;
	if (EffectiveNETD > 0.0f)
	{
		ApplyNoise(Pixels, EffectiveNETD, FrameIndex);
	}

	// Step 6A: 16F — AC banding (IR only, after temporal noise)
	if (Mode == ESensorMode::IR &&
		Cfg.ACBandingFrequency > 0.0f && Cfg.ACBandingAmplitude > 0.0f)
	{
		ApplyACBanding(Pixels, Cfg.ACBandingFrequency, Cfg.ACBandingAmplitude, FrameIndex);
	}

	// Step 7: Fixed pattern noise (static per-pixel bias)
	const float EffectiveFPN = Cfg.FixedPatternNoise * Quality.NoiseScale;
	if (EffectiveFPN > 0.0f)
	{
		ApplyFixedPatternNoise(Pixels, EffectiveFPN);
	}

	// Step 7A: 16C — Hot/dead pixel defects (after FPN, sparse stamp)
	if (Cfg.DefectPixelCount > 0 && DefectIndices.Num() > 0)
	{
		ApplyDefectPixels(Pixels);
	}

	// Step 8: Vignetting (radial corner darkening)
	const float EffectiveVignette = FMath::Clamp(Cfg.Vignetting * Quality.VignettingScale, 0.0f, 1.0f);
	if (EffectiveVignette > 0.0f)
	{
		ApplyVignetting(Pixels, EffectiveVignette);
	}

	// Step 9: Scan-line modulation
	const float EffectiveScanLine = FMath::Clamp(Cfg.ScanLineStrength * Quality.ScanLineScale, 0.0f, 1.0f);
	if (Cfg.bScanLines && EffectiveScanLine > 0.0f)
	{
		ApplyScanLines(Pixels, EffectiveScanLine);
	}

	// Step 9A: 16K — Sun glint (EO only, after vignetting)
	if (Mode == ESensorMode::EO && Cfg.SunGlintIntensity > 0.0f)
	{
		ApplySunGlint(Pixels, Cfg, Telemetry.SunElevationDeg);
	}

	// Step 10: MTF degradation — Gaussian PSF (16D) if sigma set; else legacy box blur
	const float EffectiveGaussian = FMath::Max(0.0f, Cfg.GaussianSigma * Quality.GaussianSigmaScale);
	if (EffectiveGaussian > 0.0f)
	{
		ApplyGaussianBlur(Pixels, EffectiveGaussian);
	}
	else
	{
		const int32 EffectiveBlur = FMath::Clamp(Cfg.BlurRadius + Quality.BlurRadius, 0, 8);
		if (EffectiveBlur > 0)
		{
			ApplyBoxBlur(Pixels, EffectiveBlur);
		}
	}

	// Step 10A: 16I — Platform vibration (all modes, before lens distortion)
	if (Cfg.VibrationAmplitude > 0.0f)
	{
		ApplyVibration(Pixels, Cfg.VibrationAmplitude, FrameIndex);
	}

	// Step 11: lens distortion (Phase 15B) — applied last as geometric warp
	if (bDistortionEnabled)
	{
		ApplyLensDistortion(Pixels);
	}

	// Step 11B: Phase 18D — Precipitation overlay (rain/snow CPU pixel pass)
	if (Phase18.bPrecipitation && (Phase18.RainIntensity > 0.0f || Phase18.SnowIntensity > 0.0f))
	{
		ApplyPrecipitation(Pixels, FrameIndex);
	}

	// Step 11C: Phase 18K — Dynamic IR extinction from atmospheric visibility (IR only)
	if (Phase18.bDynamicIRExtinction && Mode == ESensorMode::IR)
	{
		ApplyDynamicIRExtinction(Pixels, static_cast<float>(Telemetry.SlantRangeM));
	}

	// Step 12: 16H — Rolling shutter (EO only, very last — blends with previous frame)
	if (Mode == ESensorMode::EO && Cfg.RollingShutterStrength > 0.0f)
	{
		ApplyRollingShutter(Pixels, Cfg.RollingShutterStrength);
	}
	else if (Mode == ESensorMode::EO)
	{
		// Store current frame for next rolling shutter blend even when disabled,
		// so enabling mid-run has a valid previous frame. Only for EO mode (I3 fix).
		if (PreviousFrame.Num() != Pixels.Num())
			PreviousFrame = Pixels;
		else
			FMemory::Memcpy(PreviousFrame.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));
	}

	// Phase 20: HUD/OSD overlay (runs last, on top of all sensor effects)
	HudOverlay.Render(Pixels, Width, Height, static_cast<uint8>(Mode), Telemetry, FrameIndex);
}

// ---------------------------------------------------------------------------
// ApplyIR — BT.601 luma → IR tone curve → optional polarity inversion
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyIR(TArray<FColor>& Pixels, uint8 Polarity)
{
	const bool bBlackHot = (Polarity == 1);
	const int32 NumPixels = Width * Height;
	const uint8* Lut = IRToneCurve;

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		const int32 IdxStart    = RowStart * Width;
		const int32 IdxEnd      = RowEnd   * Width;

		for (int32 i = IdxStart; i < IdxEnd; ++i)
		{
			FColor& P = Pixels[i];
			const uint8 L = static_cast<uint8>((77u * P.R + 150u * P.G + 29u * P.B) >> 8u);
			uint8 I = Lut[L];
			if (bBlackHot) I = static_cast<uint8>(255 - I);
			P.R = I; P.G = I; P.B = I; P.A = 255;
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyNVG — BT.601 luma → gamma lift → P22 green phosphor tint
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyNVG(TArray<FColor>& Pixels)
{
	const uint8* Lut = NVGGammaCurve;

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);

		for (int32 i = RowStart * Width; i < RowEnd * Width; ++i)
		{
			FColor& P = Pixels[i];
			const uint8 L = static_cast<uint8>((77u * P.R + 150u * P.G + 29u * P.B) >> 8u);
			const uint8 I = Lut[L];
			P.R = 0;
			P.G = I;
			P.B = static_cast<uint8>(static_cast<uint32>(I) * 3u / 10u);
			P.A = 255;
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyIRExtinction — Koschmieder-style scene-wide haze toward mid-gray
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyIRExtinction(TArray<FColor>& Pixels,
                                            float Coeff, float SlantRangeM)
{
	const float extinction = 1.0f - FMath::Exp(-Coeff * SlantRangeM);
	if (extinction <= 0.0f) return;

	constexpr uint8 FogVal = 128;

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);

		for (int32 i = RowStart * Width; i < RowEnd * Width; ++i)
		{
			FColor& P = Pixels[i];
			P.R = static_cast<uint8>(P.R + (FogVal - P.R) * extinction);
			P.G = static_cast<uint8>(P.G + (FogVal - P.G) * extinction);
			P.B = static_cast<uint8>(P.B + (FogVal - P.B) * extinction);
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyNoise — add per-frame Gaussian noise from pre-baked ring buffer
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyNoise(TArray<FColor>& Pixels, float NETD, uint64 FrameIndex)
{
	const int32 NumPixels = Width * Height;
	const int32 RingSize  = 2 * NumPixels;
	const int32 FrameOff  = static_cast<int32>((FrameIndex % 2) * NumPixels);
	const float Scale     = NETD * 255.0f / 127.5f;
	const int16* Ring     = NoiseRing.GetData();

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		const int32 IdxStart    = RowStart * Width;
		const int32 IdxEnd      = RowEnd   * Width;

		for (int32 i = IdxStart; i < IdxEnd; ++i)
		{
			const int32 RingIdx = (i + FrameOff) % RingSize;
			const int32 Delta   = FMath::RoundToInt(Ring[RingIdx] * Scale);
			FColor& P = Pixels[i];
			P.R = static_cast<uint8>(FMath::Clamp(static_cast<int32>(P.R) + Delta, 0, 255));
			P.G = static_cast<uint8>(FMath::Clamp(static_cast<int32>(P.G) + Delta, 0, 255));
			P.B = static_cast<uint8>(FMath::Clamp(static_cast<int32>(P.B) + Delta, 0, 255));
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyFixedPatternNoise — static per-pixel bias (column striping / FPA defects)
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyFixedPatternNoise(TArray<FColor>& Pixels, float FPNAmplitude)
{
	const float  Scale = FPNAmplitude * 255.0f / 127.0f;
	const int16* FPN   = FixedPatternMap.GetData();

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		const int32 IdxStart    = RowStart * Width;
		const int32 IdxEnd      = RowEnd   * Width;

		for (int32 i = IdxStart; i < IdxEnd; ++i)
		{
			const int32 Delta = FMath::RoundToInt(FPN[i] * Scale);
			FColor& P = Pixels[i];
			P.R = static_cast<uint8>(FMath::Clamp(static_cast<int32>(P.R) + Delta, 0, 255));
			P.G = static_cast<uint8>(FMath::Clamp(static_cast<int32>(P.G) + Delta, 0, 255));
			P.B = static_cast<uint8>(FMath::Clamp(static_cast<int32>(P.B) + Delta, 0, 255));
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyVignetting — multiply each pixel by precomputed radial weight
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyVignetting(TArray<FColor>& Pixels, float Strength)
{
	const float* Vig = VignetteWeights.GetData();

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		const int32 IdxStart    = RowStart * Width;
		const int32 IdxEnd      = RowEnd   * Width;

		for (int32 i = IdxStart; i < IdxEnd; ++i)
		{
			// Lerp from 1.0 toward the raw radial weight by Strength
			const float W = 1.0f - Strength * (1.0f - Vig[i]);
			FColor& P = Pixels[i];
			P.R = static_cast<uint8>(FMath::RoundToInt(P.R * W));
			P.G = static_cast<uint8>(FMath::RoundToInt(P.G * W));
			P.B = static_cast<uint8>(FMath::RoundToInt(P.B * W));
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyScanLines — alternating row brightness reduction
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyScanLines(TArray<FColor>& Pixels, float Strength)
{
	// Even rows are darkened by Strength; odd rows are unaffected.
	// Strength = 0.05 → even rows at 95% brightness.
	//
	// Parallelism: only even rows are touched, so we band over the set of even
	// row indices (EvenRowCount = ceil(Height/2)).  Each band owns a disjoint
	// subset of even rows — no overlap with odd rows, safe without locks.
	const float  DimFactor    = 1.0f - Strength;
	const int32  EvenRowCount = (Height + 1) / 2;  // number of even rows (0,2,4,…)

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand  = (EvenRowCount + kParallelBands - 1) / kParallelBands;
		const int32 EvenRowStart = Band * RowsPerBand;
		const int32 EvenRowEnd   = FMath::Min(EvenRowStart + RowsPerBand, EvenRowCount);

		for (int32 EvenIdx = EvenRowStart; EvenIdx < EvenRowEnd; ++EvenIdx)
		{
			const int32 PixelStart = (EvenIdx * 2) * Width;  // Y = EvenIdx*2
			const int32 PixelEnd   = PixelStart + Width;
			for (int32 i = PixelStart; i < PixelEnd; ++i)
			{
				FColor& P = Pixels[i];
				P.R = static_cast<uint8>(FMath::RoundToInt(P.R * DimFactor));
				P.G = static_cast<uint8>(FMath::RoundToInt(P.G * DimFactor));
				P.B = static_cast<uint8>(FMath::RoundToInt(P.B * DimFactor));
			}
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyAtmosphericAttenuation — visibility-distance attenuation toward mid-gray
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyAtmosphericAttenuation(TArray<FColor>& Pixels,
                                                     float VisibilityM,
                                                     float SlantRangeM,
                                                     float Strength)
{
	if (VisibilityM <= 0.0f || SlantRangeM <= 0.0f || Strength <= 0.0f) return;
	const float NormalizedRange = SlantRangeM / VisibilityM;
	const float Atten = FMath::Clamp((1.0f - FMath::Exp(-NormalizedRange)) * Strength, 0.0f, 1.0f);
	if (Atten <= 0.0f) return;

	constexpr uint8 FogVal = 128;
	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);

		for (int32 i = RowStart * Width; i < RowEnd * Width; ++i)
		{
			FColor& P = Pixels[i];
			P.R = static_cast<uint8>(P.R + (FogVal - P.R) * Atten);
			P.G = static_cast<uint8>(P.G + (FogVal - P.G) * Atten);
			P.B = static_cast<uint8>(P.B + (FogVal - P.B) * Atten);
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyColorTemperature — Kelvin-based white balance shift
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyColorTemperature(TArray<FColor>& Pixels, float Kelvin)
{
	const float K = FMath::Clamp(Kelvin, 1000.0f, 40000.0f) / 100.0f;
	float Red = 255.0f;
	float Green = 0.0f;
	float Blue = 0.0f;

	if (K <= 66.0f)
	{
		Green = 99.4708025861f * FMath::Loge(K) - 161.1195681661f;
		Blue  = (K <= 19.0f) ? 0.0f : 138.5177312231f * FMath::Loge(K - 10.0f) - 305.0447927307f;
	}
	else
	{
		Red   = 329.698727446f * FMath::Pow(K - 60.0f, -0.1332047592f);
		Green = 288.1221695283f * FMath::Pow(K - 60.0f, -0.0755148492f);
		Blue  = 255.0f;
	}

	const float RScale = FMath::Clamp(Red / 255.0f, 0.0f, 2.0f);
	const float GScale = FMath::Clamp(Green / 255.0f, 0.0f, 2.0f);
	const float BScale = FMath::Clamp(Blue / 255.0f, 0.0f, 2.0f);

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);

		for (int32 i = RowStart * Width; i < RowEnd * Width; ++i)
		{
			FColor& P = Pixels[i];
			P.R = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(P.R * RScale), 0, 255));
			P.G = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(P.G * GScale), 0, 255));
			P.B = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(P.B * BScale), 0, 255));
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyContrastBrightness — linear contrast and brightness in RGB space
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyContrastBrightness(TArray<FColor>& Pixels,
                                                 float Contrast,
                                                 float BrightnessBias)
{
	const float Bias = BrightnessBias * 255.0f;
	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);

		for (int32 i = RowStart * Width; i < RowEnd * Width; ++i)
		{
			FColor& P = Pixels[i];
			const auto Adjust = [Contrast, Bias](uint8 V) -> uint8
			{
				const float Centered = (static_cast<float>(V) - 127.5f) * Contrast + 127.5f + Bias;
				return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Centered), 0, 255));
			};
			P.R = Adjust(P.R);
			P.G = Adjust(P.G);
			P.B = Adjust(P.B);
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyBoxBlur — separable box blur (horizontal then vertical)
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyBoxBlur(TArray<FColor>& Pixels, int32 Radius)
{
	if (Radius <= 0 || Pixels.Num() != Width * Height) return;

	TArray<FColor> Temp;
	Temp.SetNumUninitialized(Pixels.Num());

	const int32 Kernel = Radius * 2 + 1;

	// Horizontal pass
	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		for (int32 Y = RowStart; Y < RowEnd; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				int32 SumR = 0, SumG = 0, SumB = 0;
				for (int32 K = -Radius; K <= Radius; ++K)
				{
					const int32 SX = FMath::Clamp(X + K, 0, Width - 1);
					const FColor& S = Pixels[Y * Width + SX];
					SumR += S.R;
					SumG += S.G;
					SumB += S.B;
				}
				FColor& D = Temp[Y * Width + X];
				D.R = static_cast<uint8>(SumR / Kernel);
				D.G = static_cast<uint8>(SumG / Kernel);
				D.B = static_cast<uint8>(SumB / Kernel);
				D.A = 255;
			}
		}
	}, EParallelForFlags::BackgroundPriority);

	// Vertical pass
	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		for (int32 Y = RowStart; Y < RowEnd; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				int32 SumR = 0, SumG = 0, SumB = 0;
				for (int32 K = -Radius; K <= Radius; ++K)
				{
					const int32 SY = FMath::Clamp(Y + K, 0, Height - 1);
					const FColor& S = Temp[SY * Width + X];
					SumR += S.R;
					SumG += S.G;
					SumB += S.B;
				}
				FColor& D = Pixels[Y * Width + X];
				D.R = static_cast<uint8>(SumR / Kernel);
				D.G = static_cast<uint8>(SumG / Kernel);
				D.B = static_cast<uint8>(SumB / Kernel);
				D.A = 255;
			}
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// SetDistortion — build Brown-Conrady radial remap table (Phase 15B)
// ---------------------------------------------------------------------------

void FSensorPostProcess::SetDistortion(float K1, float K2)
{
	if (Width <= 0 || Height <= 0) return;

	// Skip table allocation for identity distortion
	if (FMath::IsNearlyZero(K1) && FMath::IsNearlyZero(K2))
	{
		bDistortionEnabled = false;
		DistortionRemap.Empty();
		return;
	}

	const int32 NumPixels = Width * Height;
	DistortionRemap.SetNumUninitialized(NumPixels * 2);

	const float HalfW = Width  * 0.5f;
	const float HalfH = Height * 0.5f;
	// Normalize radius so corners have r=1
	const float MaxR = FMath::Sqrt(HalfW * HalfW + HalfH * HalfH);

	for (int32 Y = 0; Y < Height; ++Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			const float nx = (X - HalfW) / MaxR;
			const float ny = (Y - HalfH) / MaxR;
			const float r2 = nx * nx + ny * ny;
			const float factor = 1.0f + K1 * r2 + K2 * r2 * r2;

			const float srcX = nx * factor * MaxR + HalfW;
			const float srcY = ny * factor * MaxR + HalfH;

			const int32 Idx = (Y * Width + X) * 2;
			DistortionRemap[Idx]     = FMath::Clamp(srcX, 0.0f, static_cast<float>(Width - 1));
			DistortionRemap[Idx + 1] = FMath::Clamp(srcY, 0.0f, static_cast<float>(Height - 1));
		}
	}

	bDistortionEnabled = true;
	UE_LOG(LogCamSim, Log, TEXT("FSensorPostProcess: lens distortion enabled K1=%.4f K2=%.4f"), K1, K2);
}

// ---------------------------------------------------------------------------
// ApplyLensDistortion — bilinear warp using precomputed remap table
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyLensDistortion(TArray<FColor>& Pixels)
{
	if (DistortionRemap.Num() != Width * Height * 2) return;

	TArray<FColor> Src = Pixels;
	const FColor* SrcData = Src.GetData();
	const float* Remap = DistortionRemap.GetData();

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);

		for (int32 Y = RowStart; Y < RowEnd; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const int32 OutIdx = Y * Width + X;
				const int32 RemapIdx = OutIdx * 2;
				const float srcX = Remap[RemapIdx];
				const float srcY = Remap[RemapIdx + 1];

				const int32 x0 = FMath::FloorToInt(srcX);
				const int32 y0 = FMath::FloorToInt(srcY);
				const int32 x1 = FMath::Min(x0 + 1, Width - 1);
				const int32 y1 = FMath::Min(y0 + 1, Height - 1);
				const float fx = srcX - x0;
				const float fy = srcY - y0;

				const FColor& P00 = SrcData[y0 * Width + x0];
				const FColor& P10 = SrcData[y0 * Width + x1];
				const FColor& P01 = SrcData[y1 * Width + x0];
				const FColor& P11 = SrcData[y1 * Width + x1];

				const float w00 = (1.0f - fx) * (1.0f - fy);
				const float w10 = fx * (1.0f - fy);
				const float w01 = (1.0f - fx) * fy;
				const float w11 = fx * fy;

				FColor& D = Pixels[OutIdx];
				D.R = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(
					P00.R * w00 + P10.R * w10 + P01.R * w01 + P11.R * w11), 0, 255));
				D.G = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(
					P00.G * w00 + P10.G * w10 + P01.G * w01 + P11.G * w11), 0, 255));
				D.B = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(
					P00.B * w00 + P10.B * w10 + P01.B * w01 + P11.B * w11), 0, 255));
				D.A = 255;
			}
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyRadianceAGC — percentile-stretch auto gain control (Phase 16A)
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyRadianceAGC(TArray<FColor>& Pixels, const FSensorModeConfig& Cfg)
{
	const int32 NumPixels = Width * Height;

	// Build 256-bin histogram over the R channel (R==G==B after waveband remap)
	int32* Hist = AGCHistogram.GetData();
	FMemory::Memzero(Hist, 256 * sizeof(int32));
	for (const FColor& P : Pixels)
		Hist[P.R]++;

	// Find low and high percentile values by walking the CDF forward
	const int32 LoTarget = FMath::Max(1, FMath::RoundToInt(Cfg.AGCLowPercentile * NumPixels));
	const int32 HiTarget = FMath::Max(1, FMath::RoundToInt(Cfg.AGCHighPercentile * NumPixels));

	int32 Lo = 0, Hi = 255;
	int32 Acc = 0;
	for (int32 b = 0; b < 256; ++b)
	{
		Acc += Hist[b];
		if (Acc >= LoTarget) { Lo = b; break; }
	}
	Acc = 0;
	for (int32 b = 0; b < 256; ++b)
	{
		Acc += Hist[b];
		if (Acc >= HiTarget) { Hi = b; break; }
	}

	if (Hi <= Lo) return;  // degenerate (uniform frame) — skip

	// 16G: AGC lag — smooth Lo/Hi over multiple frames
	if (Cfg.AGCLagFrames > 0)
	{
		const float Alpha = 1.0f / (1.0f + static_cast<float>(Cfg.AGCLagFrames));
		if (AGCSmoothedLo < 0.0f)
		{
			// First frame — initialize directly
			AGCSmoothedLo = static_cast<float>(Lo);
			AGCSmoothedHi = static_cast<float>(Hi);
		}
		else
		{
			AGCSmoothedLo = AGCSmoothedLo + Alpha * (static_cast<float>(Lo) - AGCSmoothedLo);
			AGCSmoothedHi = AGCSmoothedHi + Alpha * (static_cast<float>(Hi) - AGCSmoothedHi);
		}
		Lo = FMath::RoundToInt(AGCSmoothedLo);
		Hi = FMath::RoundToInt(AGCSmoothedHi);
		if (Hi <= Lo) return;
	}

	// Build 256-entry remap LUT
	uint8 StretchLut[256];
	const float Scale = 255.0f / static_cast<float>(Hi - Lo);
	for (int32 i = 0; i < 256; ++i)
		StretchLut[i] = static_cast<uint8>(FMath::Clamp(
			FMath::RoundToInt(static_cast<float>(i - Lo) * Scale), 0, 255));

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		for (int32 i = RowStart * Width; i < RowEnd * Width; ++i)
		{
			FColor& P = Pixels[i];
			const uint8 V = StretchLut[P.R];
			P.R = V; P.G = V; P.B = V;
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyManualGain — manual level/gain override (Phase 16A)
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyManualGain(TArray<FColor>& Pixels, float Level, float Gain)
{
	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		for (int32 i = RowStart * Width; i < RowEnd * Width; ++i)
		{
			FColor& P = Pixels[i];
			const float V = (static_cast<float>(P.R) - Level) * Gain + Level;
			const uint8 Out = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(V), 0, 255));
			P.R = Out; P.G = Out; P.B = Out;
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyQuantization — A/D bit-depth reduction with optional Bayer dither (Phase 16B)
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyQuantization(TArray<FColor>& Pixels, int32 Bits, bool bDither)
{
	const int32 Shift = 8 - Bits;
	const int32 Step  = 1 << Shift;

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		for (int32 Y = RowStart; Y < RowEnd; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				FColor& P = Pixels[Y * Width + X];
				int32 V = static_cast<int32>(P.R);

				if (bDither)
				{
					const int32 DitherVal = BayerMatrix[(Y % 4) * 4 + (X % 4)];
					V += (DitherVal * Step) / 16;
				}

				const uint8 Q = static_cast<uint8>(FMath::Clamp((V >> Shift) << Shift, 0, 255));
				P.R = Q; P.G = Q; P.B = Q;
			}
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyDefectPixels — stamp hot/dead pixel defects (Phase 16C)
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyDefectPixels(TArray<FColor>& Pixels)
{
	const int32 NumPixels = Width * Height;
	const int32 N = DefectIndices.Num();

	for (int32 i = 0; i < N; ++i)
	{
		const int32 Idx = DefectIndices[i];
		if (Idx >= 0 && Idx < NumPixels)
		{
			FColor& P = Pixels[Idx];
			P.R = DefectValues[i];
			P.G = DefectValues[i];
			P.B = DefectValues[i];
		}
	}
}

// ---------------------------------------------------------------------------
// ApplyGaussianBlur — separable Gaussian PSF (Phase 16D)
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyGaussianBlur(TArray<FColor>& Pixels, float Sigma)
{
	if (Sigma <= 0.0f || Pixels.Num() != Width * Height) return;

	const int32 Radius = FMath::Clamp(FMath::CeilToInt(3.0f * Sigma), 1, 5);
	const int32 KernelLen = 2 * Radius + 1;

	// Build 1D Gaussian kernel
	TArray<float> Kernel;
	Kernel.SetNumUninitialized(KernelLen);
	float KSum = 0.0f;
	for (int32 i = 0; i < KernelLen; ++i)
	{
		const float x = static_cast<float>(i - Radius);
		Kernel[i] = FMath::Exp(-(x * x) / (2.0f * Sigma * Sigma));
		KSum += Kernel[i];
	}
	for (float& v : Kernel) v /= KSum;

	const float* Kd = Kernel.GetData();
	TArray<FColor> Temp;
	Temp.SetNumUninitialized(Pixels.Num());

	// Horizontal pass
	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		for (int32 Y = RowStart; Y < RowEnd; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				float SR = 0.0f, SG = 0.0f, SB = 0.0f;
				for (int32 K = -Radius; K <= Radius; ++K)
				{
					const int32 SX = FMath::Clamp(X + K, 0, Width - 1);
					const FColor& S = Pixels[Y * Width + SX];
					const float W = Kd[K + Radius];
					SR += S.R * W;
					SG += S.G * W;
					SB += S.B * W;
				}
				FColor& D = Temp[Y * Width + X];
				D.R = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(SR), 0, 255));
				D.G = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(SG), 0, 255));
				D.B = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(SB), 0, 255));
				D.A = 255;
			}
		}
	}, EParallelForFlags::BackgroundPriority);

	// Vertical pass
	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		for (int32 Y = RowStart; Y < RowEnd; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				float SR = 0.0f, SG = 0.0f, SB = 0.0f;
				for (int32 K = -Radius; K <= Radius; ++K)
				{
					const int32 SY = FMath::Clamp(Y + K, 0, Height - 1);
					const FColor& S = Temp[SY * Width + X];
					const float W = Kd[K + Radius];
					SR += S.R * W;
					SG += S.G * W;
					SB += S.B * W;
				}
				FColor& D = Pixels[Y * Width + X];
				D.R = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(SR), 0, 255));
				D.G = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(SG), 0, 255));
				D.B = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(SB), 0, 255));
				D.A = 255;
			}
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyACBanding — sinusoidal horizontal stripe artifact (Phase 16F)
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyACBanding(TArray<FColor>& Pixels,
                                         float Frequency, float Amplitude,
                                         uint64 FrameIndex)
{
	// Temporal drift: 2-second cycle at 30fps
	const float Phase = FMath::Fmod(static_cast<float>(FrameIndex), 60.0f) * (2.0f * PI / 60.0f);

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		for (int32 Y = RowStart; Y < RowEnd; ++Y)
		{
			const float NormY = static_cast<float>(Y) / Height;
			const int32 Delta = FMath::RoundToInt(
				Amplitude * FMath::Sin(2.0f * PI * Frequency * NormY + Phase));
			for (int32 X = 0; X < Width; ++X)
			{
				FColor& P = Pixels[Y * Width + X];
				const uint8 V = static_cast<uint8>(FMath::Clamp(
					static_cast<int32>(P.R) + Delta, 0, 255));
				P.R = V; P.G = V; P.B = V;
			}
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyIRPointer — Gaussian bright spot for NVG IR pointer (Phase 16L)
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyIRPointer(TArray<FColor>& Pixels, const FSensorModeConfig& Cfg)
{
	const float CX = Cfg.IRPointerX * Width;
	const float CY = Cfg.IRPointerY * Height;
	const float Radius  = FMath::Max(Cfg.IRPointerRadius, 0.5f);
	const float InvTwoR2 = 1.0f / (2.0f * Radius * Radius);
	const float Peak = FMath::Clamp(Cfg.IRPointerIntensity, 0.0f, 255.0f);

	const int32 BBoxXMin = FMath::Max(0, FMath::FloorToInt(CX - 3.0f * Radius));
	const int32 BBoxXMax = FMath::Min(Width - 1, FMath::CeilToInt(CX + 3.0f * Radius));
	const int32 BBoxYMin = FMath::Max(0, FMath::FloorToInt(CY - 3.0f * Radius));
	const int32 BBoxYMax = FMath::Min(Height - 1, FMath::CeilToInt(CY + 3.0f * Radius));

	const int32 RowsInBox = BBoxYMax - BBoxYMin + 1;
	if (RowsInBox <= 0) return;

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (RowsInBox + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = BBoxYMin + Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, BBoxYMax + 1);
		for (int32 Y = RowStart; Y < RowEnd; ++Y)
		{
			const float dy = Y - CY;
			for (int32 X = BBoxXMin; X <= BBoxXMax; ++X)
			{
				const float dx = X - CX;
				const float w  = Peak * FMath::Exp(-(dx * dx + dy * dy) * InvTwoR2);
				if (w < 0.5f) continue;
				FColor& P = Pixels[Y * Width + X];
				const float NewG = FMath::Clamp(static_cast<float>(P.G) + w, 0.0f, 255.0f);
				const float GDelta = NewG - static_cast<float>(P.G);
				P.G = static_cast<uint8>(FMath::RoundToInt(NewG));
				P.B = static_cast<uint8>(FMath::Clamp(
					FMath::RoundToInt(static_cast<float>(P.B) + GDelta * 3.0f / 10.0f), 0, 255));
			}
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyThermalDrift — gradual IR baseline shift with periodic NUC (Phase 16E)
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyThermalDrift(TArray<FColor>& Pixels,
                                            const FSensorModeConfig& Cfg,
                                            float DeltaTime)
{
	// Accumulate drift
	DriftElapsedSec += DeltaTime;
	DriftAccumulatorDN += Cfg.ThermalDriftRate * DeltaTime;

	// Auto-NUC: reset drift when interval elapses
	if (Cfg.NUCIntervalSec > 0.0f && DriftElapsedSec >= Cfg.NUCIntervalSec)
	{
		DriftAccumulatorDN = 0.0f;
		DriftElapsedSec    = 0.0f;
		return;  // NUC frame — no drift applied
	}

	const int32 Offset = FMath::RoundToInt(DriftAccumulatorDN);
	if (Offset == 0) return;

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		for (int32 i = RowStart * Width; i < RowEnd * Width; ++i)
		{
			FColor& P = Pixels[i];
			const uint8 V = static_cast<uint8>(FMath::Clamp(
				static_cast<int32>(P.R) + Offset, 0, 255));
			P.R = V; P.G = V; P.B = V;
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyRollingShutter — per-row temporal blend with previous frame (Phase 16H)
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyRollingShutter(TArray<FColor>& Pixels, float Strength)
{
	const int32 NumPixels = Width * Height;
	const bool bHasPrevious = (PreviousFrame.Num() == NumPixels);

	// Store the unblended frame BEFORE blending to avoid feedback loop (I2 fix)
	TArray<FColor> Unblended = Pixels;

	if (bHasPrevious)
	{
		const FColor* PrevData = PreviousFrame.GetData();
		const int32 MaxY = FMath::Max(Height - 1, 1);  // avoid division by zero (I4 fix)

		ParallelFor(kParallelBands, [&](int32 Band)
		{
			const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
			const int32 RowStart    = Band * RowsPerBand;
			const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
			for (int32 Y = RowStart; Y < RowEnd; ++Y)
			{
				// Bottom rows are "newer" (less blend), top rows are "older" (more blend)
				const float T = (1.0f - static_cast<float>(Y) / static_cast<float>(MaxY)) * Strength;
				if (T < (1.0f / 255.0f)) continue;  // negligible blend
				for (int32 X = 0; X < Width; ++X)
				{
					const int32 Idx = Y * Width + X;
					FColor& Cur = Pixels[Idx];
					const FColor& Prev = PrevData[Idx];
					Cur.R = static_cast<uint8>(Cur.R + FMath::RoundToInt((static_cast<float>(Prev.R) - Cur.R) * T));
					Cur.G = static_cast<uint8>(Cur.G + FMath::RoundToInt((static_cast<float>(Prev.G) - Cur.G) * T));
					Cur.B = static_cast<uint8>(Cur.B + FMath::RoundToInt((static_cast<float>(Prev.B) - Cur.B) * T));
				}
			}
		}, EParallelForFlags::BackgroundPriority);
	}

	// Store the unblended frame for next iteration (not the blended output)
	if (PreviousFrame.Num() != NumPixels)
		PreviousFrame.SetNumUninitialized(NumPixels);
	FMemory::Memcpy(PreviousFrame.GetData(), Unblended.GetData(), NumPixels * sizeof(FColor));
}

// ---------------------------------------------------------------------------
// ApplyVibration — subpixel random displacement (Phase 16I)
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyVibration(TArray<FColor>& Pixels, float Amplitude, uint64 FrameIndex)
{
	if (Amplitude <= 0.0f || Pixels.Num() != Width * Height) return;

	// Deterministic per-frame displacement using FRandomStream (thread-safe)
	// CLT approximation: average 4 uniform samples for Gaussian-like distribution
	FRandomStream Rng(static_cast<int32>(FrameIndex * 7919));
	const float dx = Amplitude * ((Rng.FRand() + Rng.FRand() + Rng.FRand() + Rng.FRand()) - 2.0f);
	const float dy = Amplitude * ((Rng.FRand() + Rng.FRand() + Rng.FRand() + Rng.FRand()) - 2.0f);

	if (FMath::Abs(dx) < 0.01f && FMath::Abs(dy) < 0.01f) return;

	// Bilinear warp with flat offset (like lens distortion but uniform shift)
	TArray<FColor> Src = Pixels;
	const FColor* SrcData = Src.GetData();

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		for (int32 Y = RowStart; Y < RowEnd; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const float srcX = FMath::Clamp(static_cast<float>(X) + dx, 0.0f, static_cast<float>(Width - 1));
				const float srcY = FMath::Clamp(static_cast<float>(Y) + dy, 0.0f, static_cast<float>(Height - 1));

				const int32 x0 = FMath::FloorToInt(srcX);
				const int32 y0 = FMath::FloorToInt(srcY);
				const int32 x1 = FMath::Min(x0 + 1, Width - 1);
				const int32 y1 = FMath::Min(y0 + 1, Height - 1);
				const float fx = srcX - x0;
				const float fy = srcY - y0;

				const FColor& P00 = SrcData[y0 * Width + x0];
				const FColor& P10 = SrcData[y0 * Width + x1];
				const FColor& P01 = SrcData[y1 * Width + x0];
				const FColor& P11 = SrcData[y1 * Width + x1];

				const float w00 = (1.0f - fx) * (1.0f - fy);
				const float w10 = fx * (1.0f - fy);
				const float w01 = (1.0f - fx) * fy;
				const float w11 = fx * fy;

				FColor& D = Pixels[Y * Width + X];
				D.R = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(
					P00.R * w00 + P10.R * w10 + P01.R * w01 + P11.R * w11), 0, 255));
				D.G = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(
					P00.G * w00 + P10.G * w10 + P01.G * w01 + P11.G * w11), 0, 255));
				D.B = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(
					P00.B * w00 + P10.B * w10 + P01.B * w01 + P11.B * w11), 0, 255));
				D.A = 255;
			}
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplyGainOffsetJitter — per-frame electronic instability (Phase 16J)
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplyGainOffsetJitter(TArray<FColor>& Pixels,
                                                float GainJitter, float OffsetJitter,
                                                uint64 FrameIndex)
{
	// Deterministic jitter per frame using FRandomStream (thread-safe)
	FRandomStream Rng(static_cast<int32>(FrameIndex * 6271));
	const float GainDelta   = GainJitter * (Rng.FRand() * 2.0f - 1.0f);
	const float OffsetDelta = OffsetJitter * (Rng.FRand() * 2.0f - 1.0f);
	const float Gain = 1.0f + GainDelta;
	const float Offset = OffsetDelta;

	if (FMath::IsNearlyEqual(Gain, 1.0f, 1e-5f) && FMath::IsNearlyZero(Offset, 0.1f)) return;

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		for (int32 i = RowStart * Width; i < RowEnd * Width; ++i)
		{
			FColor& P = Pixels[i];
			const uint8 V = static_cast<uint8>(FMath::Clamp(
				FMath::RoundToInt(static_cast<float>(P.R) * Gain + Offset), 0, 255));
			P.R = V; P.G = V; P.B = V;
		}
	}, EParallelForFlags::BackgroundPriority);
}

// ---------------------------------------------------------------------------
// ApplySunGlint — brighten highlight pixels based on sun angle (Phase 16K)
// ---------------------------------------------------------------------------

void FSensorPostProcess::ApplySunGlint(TArray<FColor>& Pixels,
                                        const FSensorModeConfig& Cfg,
                                        float SunElevationDeg)
{
	// Sun factor: sin(elevation) clamped to [0, 1] — no glint below horizon
	const float SunFactor = FMath::Max(0.0f, FMath::Sin(FMath::DegreesToRadians(SunElevationDeg)));
	if (SunFactor <= 0.0f) return;

	const float Threshold = FMath::Clamp(Cfg.SunGlintThreshold, 0.0f, 254.0f);
	const float Intensity = Cfg.SunGlintIntensity * SunFactor;
	const float Spread    = FMath::Max(Cfg.SunGlintSpread, 0.1f);
	const float Range     = 255.0f - Threshold;
	if (Range <= 0.0f) return;

	ParallelFor(kParallelBands, [&](int32 Band)
	{
		const int32 RowsPerBand = (Height + kParallelBands - 1) / kParallelBands;
		const int32 RowStart    = Band * RowsPerBand;
		const int32 RowEnd      = FMath::Min(RowStart + RowsPerBand, Height);
		for (int32 i = RowStart * Width; i < RowEnd * Width; ++i)
		{
			FColor& P = Pixels[i];
			// Use max channel as brightness proxy
			const uint8 MaxCh = FMath::Max3(P.R, P.G, P.B);
			if (MaxCh <= static_cast<uint8>(Threshold)) continue;
			// Normalized excess above threshold [0, 1] raised to spread power
			const float Excess = static_cast<float>(MaxCh - static_cast<uint8>(Threshold)) / Range;
			const float Boost = Intensity * FMath::Pow(Excess, Spread);
			P.R = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(P.R + Boost), 0, 255));
			P.G = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(P.G + Boost), 0, 255));
			P.B = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(P.B + Boost), 0, 255));
		}
	}, EParallelForFlags::BackgroundPriority);
}

// -------------------------------------------------------------------------
// Phase 18 — Weather, Atmosphere & Particle Effects
// -------------------------------------------------------------------------

void FSensorPostProcess::SetPhase18Config(const FCamSimConfig::FPhase18Config& Cfg)
{
	Phase18 = Cfg;
}

void FSensorPostProcess::ApplyPrecipitation(TArray<FColor>& Pixels, uint64 FrameIndex)
{
	const float RainI = FMath::Clamp(Phase18.RainIntensity, 0.0f, 1.0f);
	const float SnowI = FMath::Clamp(Phase18.SnowIntensity, 0.0f, 1.0f);

	if (RainI <= 0.0f && SnowI <= 0.0f) return;

	// Per-frame FRandomStream seeded from FrameIndex — thread-safe, no global state.
	FRandomStream RandStream(static_cast<int32>(FrameIndex ^ 0xDEADBEEFull));

	// Rain: vertical streaks, length 4-12 px, semi-transparent white additive
	if (RainI > 0.0f)
	{
		const int32 DropCount = FMath::RoundToInt(RainI * Width * Height * 0.005f);
		for (int32 D = 0; D < DropCount; ++D)
		{
			const int32 X     = RandStream.RandRange(0, Width  - 1);
			const int32 Y0    = RandStream.RandRange(0, Height - 1);
			const int32 Len   = RandStream.RandRange(4, 12);
			const uint8 Alpha = static_cast<uint8>(RandStream.RandRange(60, 140));
			for (int32 DY = 0; DY < Len; ++DY)
			{
				const int32 Y = FMath::Clamp(Y0 + DY, 0, Height - 1);
				FColor& P = Pixels[Y * Width + X];
				P.R = static_cast<uint8>(FMath::Min(P.R + Alpha, 255));
				P.G = static_cast<uint8>(FMath::Min(P.G + Alpha, 255));
				P.B = static_cast<uint8>(FMath::Min(P.B + Alpha, 255));
			}
		}
	}

	// Snow: 3×3 bright dots
	if (SnowI > 0.0f)
	{
		const int32 FlakeCount = FMath::RoundToInt(SnowI * Width * Height * 0.003f);
		for (int32 F = 0; F < FlakeCount; ++F)
		{
			const int32 CX = RandStream.RandRange(0, Width  - 1);
			const int32 CY = RandStream.RandRange(0, Height - 1);
			const uint8 Brightness = static_cast<uint8>(RandStream.RandRange(180, 255));
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					const int32 X = FMath::Clamp(CX + DX, 0, Width  - 1);
					const int32 Y = FMath::Clamp(CY + DY, 0, Height - 1);
					FColor& P = Pixels[Y * Width + X];
					P.R = Brightness;
					P.G = Brightness;
					P.B = Brightness;
				}
			}
		}
	}
}

void FSensorPostProcess::ApplyDynamicIRExtinction(TArray<FColor>& Pixels, float SlantRangeM)
{
	// Koschmieder: Coeff = 3.912 / VisibilityM  (2% contrast threshold)
	const float VisM  = FMath::Max(Phase18.VisibilityRangeM, 100.0f);
	const float Coeff = 3.912f / VisM;
	ApplyIRExtinction(Pixels, Coeff, SlantRangeM);
}
