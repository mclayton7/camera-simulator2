// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Everything one frame of ThermalCS needs besides its input textures (ROADMAP 4A). Filled on the game thread by
 * FThermalFrameBuilder (CamSimTest/Thermal); ClipToTranslatedWorld is set on the render thread from the view.
 * CamSimThermalRef::EvaluatePixel (CamSimTest/Thermal/ThermalReference.h) documents the per-pixel maths.
 */
struct FThermalFrameParams
{
	// In-band radiance LUT: LogLut[i] = ln B(T_i), T_i = LutMinK + i / LutScale (FBandRadiance::Build).
	static constexpr int32 LutSize     = 1024;
	static constexpr float LutMinK     = 150.0f;
	static constexpr float LutMaxK     = 1000.0f;   // upper range left for 4C exhausts
	static constexpr float LutScale    = static_cast<float>(LutSize - 1) / (LutMaxK - LutMinK);
	static constexpr int32 MaxClasses  = 32;
	static constexpr int32 NumStencils = 256;

	float LogLut[LutSize] = {};

	// Classes (index < NumClasses)
	uint32 NumClasses = 1;
	float  ClassTempK[MaxClasses]      = {};   // T_class(t), K
	float  ClassEmissivity[MaxClasses] = {};
	float  ClassKFast[MaxClasses]      = {};   // K per W m^-2
	float  ClassSAbsRef[MaxClasses]    = {};   // (1 - a) S_clear(t) cloud factor, W m^-2: what the class model assumed
	uint32 TerrainClass = 0;
	uint32 WaterClass   = 1;

	// Entities: custom-stencil value -> class + offset (stencil 0 = untagged)
	uint8 StencilClass[NumStencils]   = {};
	float StencilOffsetK[NumStencils] = {};
	float EntityDepthRatio = 0.99f;            // visible when custom depth >= scene depth * ratio (reversed Z: within 1 %)

	// Geometry: translated world (cm), the camera at the origin
	FMatrix44f ClipToTranslatedWorld = FMatrix44f::Identity;   // (NDC x, NDC y, device Z, 1) -> translated world, row vector
	FVector3f  Up = FVector3f(0.0f, 0.0f, 1.0f);               // geodetic up at the camera
	uint32 bWater      = 0;                    // 0: no sea surface (ocean off) -> no water class
	float  CamHeightCm = 0.0f;                 // camera height above the still sea surface at its nadir
	float  SeaRadiusCm = 6.371e8f;             // Gaussian radius of curvature at the camera
	float  WaterBandCm = 50.0f;                // water below this height above the sea (0.5 m + max wave amplitude)

	// Sky and atmosphere
	float TairK           = 288.15f;           // air temperature now
	float SkyEpsZ         = 0.77f;             // clear-sky zenith emissivity (T_clear / T_air)^4
	float Cloud           = 0.0f;
	float SkyHemiRadiance = 0.0f;              // view-factor average in-band sky radiance (reflected by 1 - eps)
	float BetaPerCm       = 0.0f;              // path extinction per cm

	// Solar fast term
	float  KLum           = 1.0f;              // lux per W m^-2: E_pix = pi Lum / (max(BaseLum, 0.03) K_lum)
	float  EClampWm2      = 0.0f;              // E_pix clamp: 1.5 S_clear(t)
	float  KFastScale     = 1.0f;              // 0: fast term off (no base colour / no sun light: Task 1 fallback)
	uint32 bBaseColorSrgb = 0;                 // 1: GBufferC values are sRGB-encoded (Task 1 SRGB_ENCODED)

	float InputScale = 1.0f;                   // multiplies scene colour; tests only (runtime also multiplies View.OneOverPreExposure)
};

/**
 * B(T) from the LUT: T clamped to [LutMinK, LutMaxK] (NaN -> LutMinK), ln B interpolated linearly, then exp.
 * HLSL twin: LutRadiance in Shaders/Private/CamSimThermalCommon.ush (same expressions, same order).
 */
inline float ThermalLutRadiance(const float* LogLut, float TK)
{
	float T = TK;
	if (!(T >= FThermalFrameParams::LutMinK)) T = FThermalFrameParams::LutMinK;
	if (T > FThermalFrameParams::LutMaxK) T = FThermalFrameParams::LutMaxK;
	const float X = (T - FThermalFrameParams::LutMinK) * FThermalFrameParams::LutScale;
	const int32 I = FMath::Min(FMath::FloorToInt32(X), FThermalFrameParams::LutSize - 2);
	const float F = X - static_cast<float>(I);
	const float A = LogLut[I];
	return FMath::Exp(A + (LogLut[I + 1] - A) * F);
}
