// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Everything one frame of ThermalCS needs besides its input textures (ROADMAP 4A).
 * This first version holds the in-band radiance LUT only; Task 7 adds the rest.
 */
struct FThermalFrameParams
{
	/** In-band radiance LUT: LogLut[i] = ln B(T_i), T_i = LutMinK + i / LutScale (FBandRadiance::Build). */
	static constexpr int32 LutSize  = 1024;
	static constexpr float LutMinK  = 150.0f;
	static constexpr float LutMaxK  = 1000.0f;   // upper range left for 4C exhausts
	static constexpr float LutScale = static_cast<float>(LutSize - 1) / (LutMaxK - LutMinK);

	float LogLut[LutSize] = {};
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
