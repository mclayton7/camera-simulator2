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
	 *  4. optics: Optics() (distortion resample + cos^n illumination; histogram of step 3 is taken on
 *     its output) then Blur() with PsfSigmaPx (FocalPx 0 / PsfSigmaPx 0 = off)
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

	/** Optics stage (steps 1-4a): distortion resample + cos^n relative illumination.
	 *  Scene: SrcW x SrcH linear RGBA (alpha ignored), row-major. OutRgb: W x H linear RGB, row-major
	 *  (interleaved, unlike the planar detector images). OutHist: histogram of the noiseless signal
	 *  s = dot(rgb, SignalWeights) of every OutRgb pixel (after illumination, before blur).
	 *
	 *  Contract for the GPU port (Task 9) — mirror exactly:
	 *  - Texel t(x, y) = sanitize(Scene(x, y).rgb) * InputScale for 0 <= x < SrcW, 0 <= y < SrcH;
	 *    t = 0 for any other (x, y) (out-of-bounds taps read black, per tap).
	 *  - Pixel centres are at integer + 0.5. For output pixel (px, py):
	 *      FocalPx <= 0 (optics off): s = 1, illum = 1,
	 *        sx = (px + 0.5) * SrcW / W - 0.5,  sy = (py + 0.5) * SrcH / H - 0.5
	 *        (SrcW == W and SrcH == H: the texel (px, py) is copied, no filtering).
	 *      FocalPx > 0:
	 *        xd = (px + 0.5 - W/2) / FocalPx,  yd = (py + 0.5 - H/2) / FocalPx,  rd = sqrt(xd^2 + yd^2)
	 *        ru = CamSimOptics::UndistortRadius(rd, K1, K2) final iterate (convergence is a config
	 *             check, Task 10; the sample uses the iterate either way);  s = rd > 0 ? ru / rd : 1
	 *        sx = (xd * s * FocalPx + W/2) * SrcW / W - 0.5,  sy = (yd * s * FocalPx + H/2) * SrcH / H - 0.5
	 *        (production SrcW == W: sx = xd * s * FocalPx + W/2 - 0.5)
	 *        illum = pow(1 / sqrt(1 + (xd s)^2 + (yd s)^2), VignettingExponent)   (cos^n of the ray angle)
	 *  - Bilinear: x0 = floor(sx), y0 = floor(sy), fx = sx - x0, fy = sy - y0,
	 *      c = (1 - fy) * ((1 - fx) t(x0, y0) + fx t(x0 + 1, y0)) + fy * ((1 - fx) t(x0, y0 + 1) + fx t(x0 + 1, y0 + 1))
	 *  - rgb = c * illum;  histogram[BinOf(dot(rgb, SignalWeights))]++. */
	void Optics(const TArray<FLinearColor>& Scene, int32 SrcW, int32 SrcH, int32 W, int32 H,
		const FSensorFrameParams& P, TArray<FVector3f>& OutRgb, FSensorHistogram& OutHist);

	/** Separable Gaussian PSF (step 4b) on a W x H interleaved image, in place. Taps from
	 *  CamSimOptics::PsfTaps(SigmaPx) (radius R = min(ceil(3 sigma), 8), Taps[0] centre, normalised).
	 *  Horizontal pass first, then vertical, each with clamp-to-edge addressing:
	 *    out(x) = Taps[0] in(x) + sum_{k=1..R} Taps[k] (in(clamp(x - k, 0, W-1)) + in(clamp(x + k, 0, W-1)))
	 *  accumulated in that order, float. SigmaPx <= 0 leaves the image untouched. */
	void Blur(TArray<FVector3f>& InOut, int32 W, int32 H, float SigmaPx);

	/** Scene: W*H linear RGBA (alpha ignored). W % 4 == 0, H % 2 == 0. The full physical model. */
	FResult Run(const TArray<FLinearColor>& Scene, int32 W, int32 H, const FSensorFrameParams& P);

	/** 3B.1 display path (no optics, no detector): EO rgb = Oetf709(Knee(c * PhotonGain * AnalogGain)),
	 *  IR v = saturate(DisplayGain * s * PhotonGain * AnalogGain + DisplayOffset). What the GPU graph
	 *  computes until Task 9 ports the detector; CamSim.GPU.Sensor.* compare against it until then. */
	FResult RunDisplayOnly(const TArray<FLinearColor>& Scene, int32 W, int32 H, const FSensorFrameParams& P);
}
