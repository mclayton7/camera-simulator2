// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FSensorOpticsConfig;

/**
 * Lens-model helpers (ROADMAP 3B.2). Coordinates are normalised by the focal length in pixels:
 * a pixel at offset (dx, dy) px from the image centre has normalised position (dx, dy) / FocalPx,
 * i.e. tan of its field angle. Distortion is radial Brown-Conrady from ideal (undistorted) radius
 * ru to distorted radius rd:  rd = ru (1 + K1 ru^2 + K2 ru^4).
 * The image the sensor sees is distorted; output pixels are therefore at distorted positions and
 * each one samples the ideal (rendered) scene at ru — see CamSimSensorRef::Optics.
 */
namespace CamSimOptics
{
	/** Newton iterations of UndistortRadius — fixed so the GPU runs the identical recurrence. */
	constexpr int32 NewtonIterations = 3;
	/** |ru (1 + K1 ru^2 + K2 ru^4) - rd| above this after NewtonIterations = not converged. */
	constexpr float NewtonTolerance = 1e-5f;
	/** Largest PSF tap radius (px); larger sigmas are truncated. */
	constexpr int32 MaxPsfRadius = 8;

	/** Focal length in pixels for an output width and horizontal FOV: (W/2) / tan(HFOV/2). */
	float FocalPx(int32 Width, float HFovDeg);

	/** Ideal (undistorted) normalised radius for a distorted one. Newton from r = Rd:
	 *    r <- r - (r (1 + K1 r^2 + K2 r^4) - Rd) / (1 + 3 K1 r^2 + 5 K2 r^4),   NewtonIterations times
	 *  OutRu is always the final iterate (what the GPU uses). Returns false when the result does not
	 *  satisfy the forward model (|residual| > NewtonTolerance), is not finite, or lies where the
	 *  forward model is not monotonic (derivative <= 0: the lens folds the image back on itself). */
	bool UndistortRadius(float Rd, float K1, float K2, float& OutRu);

	/** Largest distorted radius in the frame, normalised by FocalPx: hypot(W/2, H/2) / FocalPx. */
	float CornerRadius(int32 W, int32 H, float FocalPx);

	/** PSF sigma in pixels: sqrt((0.42 * lambda * N / pitch)^2 + 0.29^2 + extra^2).
	 *  0.42 lambda N is the Gaussian fit to the Airy core; 0.29 ~ 1/sqrt(12) is the pixel aperture. */
	float PsfSigmaPx(const FSensorOpticsConfig& O);

	/** Normalised 1-D Gaussian taps, radius R = min(ceil(3 sigma), MaxPsfRadius); OutTaps has R + 1
	 *  entries, Taps[0] the centre, Taps[k] = exp(-k^2 / (2 sigma^2)) / (T0 + 2 sum_{k>=1} Tk).
	 *  sigma <= 0 gives the single tap { 1 }. */
	void PsfTaps(float SigmaPx, TArray<float>& OutTaps);
}
