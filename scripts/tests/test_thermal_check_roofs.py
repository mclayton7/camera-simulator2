"""Unit tests for gates s / s_air (roofs, REALISM R3b) in scripts/thermal_check.py."""

import math

import numpy as np
import thermal_check as tc


def _sources(lat0, lon0):
    kx = 111_320.0 * math.cos(math.radians(lat0))

    def masked(lon, lat):  # a building over the east half
        return (np.asarray(lon) - lon0) * kx > 0.0

    def codes(lon, lat):  # grassland everywhere
        return np.full(np.shape(lon), 30, np.uint8)

    return masked, codes


def test_roof_masks_geometry():
    masked, codes = _sources(33.3, -117.3)
    roof, grass = tc.roof_masks((200, 300), 0.5, 33.3, -117.3, masked, codes)
    cols_r, cols_g = np.nonzero(roof.any(axis=0))[0], np.nonzero(grass.any(axis=0))[0]
    centre = (300 - 1) / 2.0
    assert cols_r.min() * 1.0 > centre + tc.ROOF_ERODE_M / 0.5 - 1  # eroded inside the footprint
    assert cols_g.max() * 1.0 < centre - tc.ROOF_GRASS_CLEAR_M / 0.5 + 1  # grass at least 10 m from it
    assert not (roof & grass).any() and roof.sum() > 0 and grass.sum() > 0
    assert not (roof | grass)[:, :30].any()  # central 60 % only


def test_roof_row_pass_fail_and_minimum_area():
    masked, codes = _sources(33.3, -117.3)
    masks = tc.roof_masks((200, 300), 0.5, 33.3, -117.3, masked, codes)
    y = np.full((200, 300), 100.0)
    y[masks[0]] = 108.0
    assert tc.roof_row("mwir", "noon", y, masks, 3.0)["pass"]
    assert not tc.roof_row("mwir", "noon", y, masks, 10.0)["pass"]
    tiny = (np.zeros_like(masks[0]), masks[1])
    tiny[0][100, 200] = True
    assert not tc.roof_row("mwir", "noon", y, tiny, 3.0)["pass"]  # one roof pixel is not a measurement


def test_roof_air_temps_parse_the_thermal_log():
    temps = " ".join(f"{280.0 + k:.1f}" for k in range(16))
    night = f"LogCamSim: Thermal: classes[K] {temps} terrain=0 water=1 Tair=284.3 K cloud=0.00 epsZ=0.800 sunLux=0 x"
    noon = night.replace("Tair=284.3", "Tair=291.0").replace("sunLux=0", "sunLux=90000")
    out = tc.roof_air_temps([night, noon])
    assert out["night"] == (295.0, 288.0, 284.3) and out["noon"][2] == 291.0
    assert tc.roof_air_temps(["no thermal line"]) == {}


def test_expected_rows_include_the_roof_gates():
    rows = tc.expected_rows(["mwir"], {"roofs"})
    assert rows == [("s", "mwir", "noon"), ("s_air", "mwir", "night")]
    assert tc.expected_rows(["lwir"], {"roofs"}) == []
    assert tc.run_group("mwir_roofs") == "roofs" and tc.run_band("mwir_roofs") == "mwir"
