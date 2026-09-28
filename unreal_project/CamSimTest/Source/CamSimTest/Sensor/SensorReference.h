// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SensorFrameParams.h"

/** CPU reference of the sensor graph (ROADMAP 3B.2). Images are planar floats: W*H per channel,
 *  channel c at [c*W*H, (c+1)*W*H). Randomness comes from CamSimHash (SensorHash.h). */
namespace CamSimSensorRef
{
	/** Order of operations of Run():
	 *  1. c = sanitize(scene colour): NaN -> 0, clamp to [0, 65504]
	 *  2. c *= InputScale (runtime: View.OneOverPreExposure)
	 *  3. s = dot(c, SignalWeights); histogram[BinOf(s)]++   (noiseless signal)
	 *  4. optics (distortion, vignetting, PSF blur) — pass-through until Task 8 (FocalPx 0 / PsfSigmaPx 0 = off)
	 *  5. detector + ADC (DetectPixel) on EO rgb (3 channels) or IR s (1 channel) -> DN
	 *  6. display on n = DN / AdcMax:
	 *     EO: rgb = Oetf709(Knee(n, KneeStart))                                   (per channel)
	 *     IR: v = saturate(n * DisplayGain + DisplayOffset); if (bBlackHot) v = 1 - v;  rgb = v, luma source = v
	 *  7. NV12 (BT.709 limited range): per 4x2 block, Y per pixel, U/V from the 2x2 mean of R'G'B';
	 *     IR: Y from v, U = V = 128. */
	struct FResult
	{
		TArray<uint8>    Nv12;       // W*H*3/2
		FSensorHistogram Histogram;
	};

	float Sanitize(float V);
	/** Soft highlight knee, identity below K; for X > K:
	 *  K + (1 - K) * (1 - exp(-(X - K) / (1 - K))) / (1 - exp(-1)), so Knee(1) = 1. */
	float Knee(float X, float K);
	float Oetf709(float L);

	/** Detector + ADC for one channel: signal fraction in, DN (float, integer-valued, [0, AdcMax]) out.
	 *  Photon (DetectorType 0), streams offset by Channel * 16:
	 *    e  = Signal * PhotonGain * FullWellE
	 *    e1 = e * (1 + Prnu * G(x,y,Fixed,1))
	 *    e2 = e1 + DarkE;  e2 += sqrt(max(e2, 0)) * G(x,y,Frame,2)        (shot noise on signal + dark)
	 *    e3 = e2 + DsnuE * G(x,y,Fixed,3)
	 *    e4 = e3 + ReadNoiseE * G(x,y,Frame,4)
	 *    e5 = clamp(e4, 0, FullWellE) * AnalogGain
	 *    DN = clamp(floor(e5 * AdcMax / FullWellE + 0.5), 0, AdcMax)
	 *  Microbolometer (DetectorType 1):
	 *    v  = Signal * PhotonGain + TemporalNoise*G(x,y,Frame,5) + PixelFpn*G(x,y,Fixed,6)
	 *         + ColumnFpn*G(x,0,Fixed,7) + RowFpn*G(0,y,Fixed,8)
	 *    DN = clamp(floor(v * AdcMax + 0.5), 0, AdcMax)
	 *  Defects (both): u = Uniform(Hash(x,y,Fixed,Seed,2*9)); u < HotFraction -> AdcMax; u > 1 - DeadFraction -> 0.
	 *  A NaN signal is treated as 0 (no clamp ever sees NaN). */
	float DetectPixel(float Signal, int32 X, int32 Y, uint32 Channel, const FSensorFrameParams& P);

	/** DetectPixel over a planar image of Channels (1 or 3) channels; returns planar DN. */
	TArray<float> DetectImage(const TArray<float>& SignalRgbOrMono, int32 W, int32 H, int32 Channels, const FSensorFrameParams& P);

	/** Display of normalised DN (DN / AdcMax) -> [0, 1]. */
	float DisplayEo(float N, const FSensorFrameParams& P);
	float DisplayIr(float N, const FSensorFrameParams& P);

	/** Scene: W*H linear RGBA (alpha ignored). W % 4 == 0, H % 2 == 0. The full physical model. */
	FResult Run(const TArray<FLinearColor>& Scene, int32 W, int32 H, const FSensorFrameParams& P);

	/** 3B.1 display path (no optics, no detector): EO rgb = Oetf709(Knee(c * PhotonGain * AnalogGain)),
	 *  IR v = saturate(DisplayGain * s * PhotonGain * AnalogGain + DisplayOffset). What the GPU graph
	 *  computes until Task 9 ports the detector; CamSim.GPU.Sensor.* compare against it until then. */
	FResult RunDisplayOnly(const TArray<FLinearColor>& Scene, int32 W, int32 H, const FSensorFrameParams& P);
}
