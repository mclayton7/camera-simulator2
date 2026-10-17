# Copyright CamSim Contributors. All Rights Reserved.
"""Ozone (Chappuis band) absorption for UE's SkyAtmosphere "other absorption" layer, integrated over the linear sRGB
channels (ROADMAP 6A, docs/night.md "Twilight colour"). Prints the per-km coefficients at the tent's peak density that
FNightIllumination::OzoneAbsorptionPerKm() hard-codes.

UE's default (0.000650, 0.001881, 0.000085) /km is Bruneton's (2017) cross section sampled at 680, 550 and 440 nm. A
renderer working in linear sRGB needs the band integral instead: the sRGB red channel peaks near 600 nm, the Chappuis
peak, so ozone absorbs red most (R/G ~ 1.39), not green (R/G 0.35). With the monochromatic values, the long ozone paths
of twilight (sun below the horizon) filter the sky magenta instead of blue.

Method: for a 5,778 K sun, the linear sRGB colour (CIE 1931 2 deg observer, IEC 61966-2-1 matrix) of the spectrum after
an ozone path of p km at peak density, divided by the unfiltered colour, is fitted per channel by exp(-k p) (least
squares on -ln T over p in 0.5 .. 300 km: from one zenith column, ~15 km, to the grazing twilight paths). A slightly
negative blue fit (the sRGB matrix's negative lobe) is clamped to 0.

    uv run --with colour-science --with numpy python scripts/sky/ozone_rgb.py
"""

import colour
import numpy as np

# Bruneton, "Precomputed Atmospheric Scattering: a New Implementation" (2017), demo.cc kOzoneCrossSection: IUP Bremen
# (Serdyuchenko et al. 2014) O3 cross sections at 233 K averaged over 10 nm bins from 360 nm, m^2.
OZONE_M2 = np.array([
    1.18e-27, 2.182e-28, 2.818e-28, 6.636e-28, 1.527e-27, 2.763e-27, 5.52e-27, 8.451e-27, 1.582e-26, 2.316e-26,
    3.669e-26, 4.924e-26, 7.752e-26, 9.016e-26, 1.48e-25, 1.602e-25, 2.139e-25, 2.755e-25, 3.091e-25, 3.5e-25,
    4.266e-25, 4.672e-25, 4.398e-25, 4.701e-25, 5.019e-25, 4.305e-25, 3.74e-25, 3.215e-25, 2.662e-25, 2.238e-25,
    1.852e-25, 1.473e-25, 1.209e-25, 9.423e-26, 7.455e-26, 6.566e-26, 5.105e-26, 4.15e-26, 4.228e-26, 3.237e-26,
    2.451e-26, 2.801e-26, 2.534e-26, 1.624e-26, 1.465e-26, 2.078e-26, 1.383e-26, 7.105e-27])
# 300 Dobson units spread over UE's tent (peak at 25 km, 15 km half width: 15 km of peak-density equivalent)
PEAK_PER_M3 = 300.0 * 2.687e20 / 15000.0
XYZ_TO_SRGB = np.array([[3.2404542, -1.5371385, -0.4985314], [-0.9692660, 1.8760108, 0.0415560],
                        [0.0556434, -0.2040259, 1.0572252]])


def main() -> None:
    lam = np.arange(360.0, 831.0, 1.0)
    cmfs = colour.MSDS_CMFS["CIE 1931 2 Degree Standard Observer"].copy().align(colour.SpectralShape(360, 830, 1)).values
    sigma_km = np.interp(lam, np.arange(360.0, 831.0, 10.0), OZONE_M2) * PEAK_PER_M3 * 1000.0
    lm = lam * 1e-9
    sun = 1.0 / lm**5 / (np.exp(1.4388e-2 / (lm * 5778.0)) - 1.0)

    def rgb(spec: np.ndarray) -> np.ndarray:
        return XYZ_TO_SRGB @ (cmfs.T @ spec)

    ref = rgb(sun)
    paths = np.linspace(0.5, 300.0, 60)
    trans = np.array([rgb(sun * np.exp(-sigma_km * p)) / ref for p in paths])
    k = [max(float(paths @ -np.log(np.clip(trans[:, c], 1e-12, None)) / (paths @ paths)), 0.0) for c in range(3)]
    mono = np.interp([680.0, 550.0, 440.0], lam, sigma_km)
    print("UE default (680/550/440 nm) /km: %.6g %.6g %.6g" % tuple(mono))
    print("sRGB band /km:                   %.6g %.6g %.6g (R/G %.3f)" % (k[0], k[1], k[2], k[0] / k[1]))


if __name__ == "__main__":
    main()
