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

	UE_LOG(LogCamSim, Log,
		TEXT("FSensorPostProcess: initialized %dx%d ring=%d EO/IR/NVG=%d/%d/%d quality(noise=%.2f vignette=%.2f scan=%.2f atmos=%.2f blur=%d contrast=%.2f bias=%.2f)"),
		Width, Height, RingSize,
		Configs.Contains(ESensorMode::EO) ? 1 : 0,
		Configs.Contains(ESensorMode::IR) ? 1 : 0,
		Configs.Contains(ESensorMode::NVG) ? 1 : 0,
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

	// Step 7: Fixed pattern noise (static per-pixel bias)
	const float EffectiveFPN = Cfg.FixedPatternNoise * Quality.NoiseScale;
	if (EffectiveFPN > 0.0f)
	{
		ApplyFixedPatternNoise(Pixels, EffectiveFPN);
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

	// Step 10: blur (mode + quality)
	const int32 EffectiveBlur = FMath::Clamp(Cfg.BlurRadius + Quality.BlurRadius, 0, 8);
	if (EffectiveBlur > 0)
	{
		ApplyBoxBlur(Pixels, EffectiveBlur);
	}
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
