// Copyright CamSim Contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sensor/SensorTypes.h"

/**
 * Radiometric EO exposure (ROADMAP 9.4, 6A). For a photon detector with radiometric optics (transmission and QE set,
 * the eo_hd_cmos preset) the photon gain is an integration time:
 *
 *   R          = π / (4 N²) · τ · A_pix · QE · photons_per_lm      [electrons / s per cd/m²]
 *   PhotonGain = R · t_int / FullWellE
 *
 * so the AE's photon-stage limits come from [integration_min_us, integration_max_ms] instead of the calibrated
 * exposure.min_gain_ev / max_photon_gain_ev. The sensor shader and CPU reference are unchanged (e = Signal ·
 * PhotonGain · FullWellE). Other modes (IR, or a photon detector without transmission/QE) keep their calibrated gain.
 */
namespace CamSimRadiometry
{
	/** Photon detector with lens transmission > 0 and QE > 0. */
	bool IsRadiometric(const FSensorModeConfig& M);

	/** Photons per lumen of a 5800 K blackbody over [BandLoUm, BandHiUm] µm (1 nm Planck sum; CIE photopic V(λ)
	 *  approximated by a Gaussian). ≈ 1.09e16 for 0.4–0.7 µm; 0 for a band with no photopic response. Cached per band:
	 *  game thread only. */
	double PhotonsPerLumen(float BandLoUm, float BandHiUm);

	/** R = π/(4N²) τ A_pix QE · photons/lm: electrons per second per cd/m² of scene luminance (0 if not radiometric). */
	double ElectronsPerSecondPerNit(const FSensorModeConfig& M);

	/** The exposure M's AE uses: radiometric modes get MinGainEv/MaxPhotonGainEv from t_min/t_max, others M.Exposure unchanged. */
	FSensorExposureConfig EffectiveExposure(const FSensorModeConfig& M);

	/** Integration time (s) of a photon gain: t = PhotonGain · FullWellE / R (0 if not radiometric). */
	double IntegrationSeconds(const FSensorModeConfig& M, float PhotonGain);
}
