// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SensorFrameParams.h"

namespace CamSimSensorRef
{
	/** Order of operations — the GPU kernels (CamSimSensor.usf) follow this exactly:
	 *  1. c = sanitize(scene colour): NaN -> 0, clamp to [0, 65504]
	 *  2. c *= InputScale (runtime: View.OneOverPreExposure)
	 *  3. s = dot(c, SignalWeights); histogram[BinOf(s)]++
	 *  4. EO: rgb = Oetf709(saturate(Knee(c * Gain, KneeStart)))           (per channel)
	 *     IR: v = saturate(s * Gain + Offset); if (bBlackHot) v = 1 - v;   rgb = v, luma source = v
	 *  5. NV12 (BT.709 limited range): per 4x2 block, Y per pixel, U/V from the 2x2 mean of R'G'B';
	 *     IR: Y from v, U = V = 128. */
	struct FResult
	{
		TArray<uint8>    Nv12;       // W*H*3/2
		FSensorHistogram Histogram;
	};

	float Sanitize(float V);
	float Knee(float X, float K);
	float Oetf709(float L);

	/** Scene: W*H linear RGBA (alpha ignored). W % 4 == 0, H % 2 == 0. */
	FResult Run(const TArray<FLinearColor>& Scene, int32 W, int32 H, const FSensorFrameParams& P);
}
