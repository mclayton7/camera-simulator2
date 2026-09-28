// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SensorFrameParams.h"

/** CPU reference of the sensor graph (ROADMAP 3B.2). Images are planar floats: W*H per channel,
 *  channel c at [c*W*H, (c+1)*W*H). Randomness comes from CamSimHash (SensorHash.h). */
namespace CamSimSensorRef
{
	/** Order of operations of Run(). The GPU graph (CamSimShaders/SensorGraph.h: one fused SensorCS pass,
	 *  Shaders/Private/CamSimSensor.usf) follows it exactly, expression for expression:
	 *  1. texel t = sanitize(scene colour) (NaN -> 0, clamp to [0, 65504]) * InputScale
	 *     (runtime: InputScale * View.OneOverPreExposure); bloom texels likewise
	 *  2. [+ bloom]: c = scene sample + bloom sample at the same normalised position (Optics())
	 *  3. distortion resample (Optics(); FocalPx 0 = off) and cos^n relative illumination
	 *  4. s = dot(c, SignalWeights); histogram[BinOf(s)]++   (noiseless signal, after illumination)
	 *  5. PSF blur, Blur() with P.PsfTaps (optical sigma, pixel-integrated; NumPsfTaps <= 1 = off)
	 *  6. detector + ADC + defects (DetectPixel) on EO rgb (3 channels) or IR s (1 channel) -> DN
	 *  7. display on n = DN * (1 / AdcMax):
	 *     EO: rgb = Oetf709(Knee(n, KneeStart))                                   (per channel)
	 *     IR: v = saturate(n * DisplayGain + DisplayOffset); if (bBlackHot) v = 1 - v;  rgb = v, luma source = v
	 *  8. NV12 (BT.709 limited range, floor(x + 0.5)): per 4x2 block, Y per pixel, U/V from the 2x2 mean
	 *     of R'G'B'; IR: Y from v, U = V = 128. */
	struct FResult
	{
		TArray<uint8>    Nv12;       // W*H*3/2
		FSensorHistogram Histogram;
	};

	/** A row-major linear RGBA image (alpha ignored): a render's view rect, or its bloom's. */
	struct FImage
	{
		const TArray<FLinearColor>* Texels = nullptr;
		int32 W = 0, H = 0;
		bool IsValid() const { return Texels && W > 0 && H > 0 && Texels->Num() == W * H; }
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

	/** Optics stage (steps 1-4): distortion resample + cos^n relative illumination.
	 *  Scene: SrcW x SrcH linear RGBA (alpha ignored), row-major. OutRgb: W x H linear RGB, row-major
	 *  (interleaved, unlike the planar detector images). OutHist: histogram of the noiseless signal
	 *  s = dot(rgb, SignalWeights) of every OutRgb pixel (after illumination, before blur).
	 *
	 *  Contract the GPU mirrors exactly (CamSimSensor.usf OpticsAt):
	 *  - OutHist is reset (all bins 0, Serial 0) at entry, then counts every output pixel once.
	 *  - Texel t(x, y) = sanitize(Scene(x, y).rgb) * InputScale.
	 *  - Pixel centres are at integer + 0.5. For output pixel (px, py):
	 *      FocalPx <= 0 (optics off): s = 1, illum = 1,
	 *        sx = (px + 0.5) * SrcW / W - 0.5,  sy = (py + 0.5) * SrcH / H - 0.5
	 *        (SrcW == W and SrcH == H: the texel (px, py) is copied, no filtering).
	 *      FocalPx > 0:
	 *        xd = (px + 0.5 - W/2) / FocalPx,  yd = (py + 0.5 - H/2) / FocalPx,  rd = sqrt(xd^2 + yd^2)
	 *        ru = CamSimOptics::UndistortRadius(rd, K1, K2) final iterate (convergence is a config
	 *             check, CamSimOptics::DistortionConverges; the sample uses the iterate either way);  s = rd > 0 ? ru / rd : 1
	 *        sx = (xd * s * FocalPx + W/2) * SrcW / W - 0.5,  sy = (yd * s * FocalPx + H/2) * SrcH / H - 0.5
	 *        (production SrcW == W: sx = xd * s * FocalPx + W/2 - 0.5)
	 *        illum = pow(1 / sqrt(1 + (xd s)^2 + (yd s)^2), VignettingExponent)   (cos^n of the ray angle)
	 *  - Footprint: a sample with sx outside [-0.5, SrcW - 0.5] or sy outside [-0.5, SrcH - 0.5]
	 *    (inclusive bounds) is black, c = 0 — it looks outside the render.
	 *  - Otherwise bilinear with taps clamped to the valid range:
	 *      x0 = floor(sx), y0 = floor(sy), fx = sx - x0, fy = sy - y0,
	 *      xa = clamp(x0, 0, SrcW-1), xb = clamp(x0 + 1, 0, SrcW-1), ya = clamp(y0, 0, SrcH-1), yb = clamp(y0 + 1, 0, SrcH-1)
	 *      c = (1 - fy) * ((1 - fx) t(xa, ya) + fx t(xb, ya)) + fy * ((1 - fx) t(xa, yb) + fx t(xb, yb))
	 *  - Bloom (optional, BW x BH; the GPU reads it with integer Loads at the same clamped taps, offset
	 *    by the bloom view rect's origin): inside
	 *    the footprint, c += bilinear of the bloom texels b = sanitize(Bloom) * InputScale at
	 *      bx = (sx + 0.5) * (BW / SrcW) - 0.5,  by = (sy + 0.5) * (BH / SrcH) - 0.5
	 *    (the same normalised position; BW / SrcW is one float division), taps clamped to [0, BW-1] x
	 *    [0, BH-1] like the scene's, no footprint test of its own. The same-size copy above is sx = px.
	 *  - rgb = c * illum (FocalPx > 0 only);  OutHist[BinOf(dot(rgb, SignalWeights))]++. */
	void Optics(const TArray<FLinearColor>& Scene, int32 SrcW, int32 SrcH, int32 W, int32 H,
		const FSensorFrameParams& P, TArray<FVector3f>& OutRgb, FSensorHistogram& OutHist, const FImage& Bloom = FImage());

	/** Separable Gaussian PSF (step 5) on a W x H interleaved image, in place. Taps are P.PsfTaps
	 *  (CamSimOptics::SetPsf from the OPTICAL sigma: pixel-integrated, radius R = NumPsfTaps - 1 <= 8,
	 *  Taps[0] centre, normalised) — the same constants the GPU blur gets.
	 *  Horizontal pass first, then vertical, each with clamp-to-edge addressing:
	 *    out(x) = Taps[0] in(x) + sum_{k=1..R} Taps[k] (in(clamp(x - k, 0, W-1)) + in(clamp(x + k, 0, W-1)))
	 *  accumulated in that order, float. NumPsfTaps <= 1 leaves the image untouched. */
	void Blur(TArray<FVector3f>& InOut, int32 W, int32 H, const FSensorFrameParams& P);

	/** Scene: W*H linear RGBA (alpha ignored). W % 4 == 0, H % 2 == 0. The full physical model. */
	FResult Run(const TArray<FLinearColor>& Scene, int32 W, int32 H, const FSensorFrameParams& P);

	/** The full model on a Scene of any size (resampled per Optics()) plus optional Bloom, to a W x H output. */
	FResult Run(const FImage& Scene, const FImage& Bloom, int32 W, int32 H, const FSensorFrameParams& P);
}
