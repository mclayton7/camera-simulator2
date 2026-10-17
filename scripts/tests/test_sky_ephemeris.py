"""scripts/sky/ephemeris.py: the Python mirror of FEphemeris / FNightIllumination, held to the same JPL DE421 rows
and tolerances as CamSim.Environment.Ephemeris.* (Tests/EphemerisTest.cpp)."""

import math
import sys
from datetime import datetime, timezone
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sky"))
import ephemeris as eph  # noqa: E402

# (y, m, d, h, mi, lat, lon, sunAz, sunEl, moonAz, moonEl, illum, moonKm, LAST) from EphemerisTest.cpp
ROWS = [
    (2026, 10, 16, 6, 0, 33.3, -117.35, 312.6906, -57.2202, 246.7443, -16.7400, 0.2800, 406154, 357.4261),
    (2026, 10, 26, 7, 0, 33.3, -117.35, 337.9491, -67.8194, 153.1094, 72.1309, 0.9982, 362405, 22.3236),
    (2026, 11, 24, 8, 0, 37.77, -122.42, 2.9229, -72.7814, 204.0726, 75.5026, 0.9969, 355309, 60.8785),
    (2027, 3, 22, 10, 0, 32.65, -114.6, 50.4250, -44.0245, 229.6138, 39.8970, 0.9991, 368287, 215.0879),
    (2026, 6, 21, 12, 0, 78.22, 15.65, 197.0215, 34.7579, 110.1330, 3.2140, 0.4583, 385691, 105.3515),
    (2026, 12, 21, 0, 0, -33.87, 151.21, 74.5200, 63.2207, 122.1706, -69.8726, 0.8656, 373023, 240.7928),
    (2024, 4, 8, 18, 18, 23.0, -105.0, 144.0552, 71.3638, 143.9309, 71.3881, 0.0000, 353753, 6.9962),
]


def ang_diff(a: float, b: float) -> float:
    return abs((a - b + 180.0) % 360.0 - 180.0)


@pytest.mark.parametrize("row", ROWS)
def test_matches_de421_rows(row):
    y, mo, d, h, mi, lat, lon, saz, sel, maz, mel, illum, km, last = row
    e = eph.compute(datetime(y, mo, d, h, mi, tzinfo=timezone.utc), lat, lon, 0.0)
    assert abs(e.sun_el - sel) < 0.05
    if sel > -85.0:
        assert ang_diff(e.sun_az, saz) * math.cos(math.radians(sel)) < 0.05
    assert abs(e.moon_el - mel) < 0.2
    assert ang_diff(e.moon_az, maz) * math.cos(math.radians(mel)) < 0.2
    assert abs(e.moon_illum - illum) < 0.01
    assert abs(e.moon_km - km) < 300.0
    assert ang_diff(e.last_deg, last) < 0.01


def test_full_moon_at_zenith_lights_the_ground_like_the_cpp_model():
    # NightIlluminationTest: full moon at zenith ~0.25-0.3 lux at the ground
    toa = eph.moon_toa_lux(0.0, 384400.0)
    assert 0.29 < toa < 0.33
    ground = toa * eph.transmittance(90.0)
    assert 0.25 < ground < 0.3
    assert eph.transmittance(-1.0) == 0.0


def test_moon_shade_lit_limb_faces_the_sun():
    assert eph.moon_shade(0.9, 0.0, 90.0) > 0.5
    assert eph.moon_shade(-0.9, 0.0, 90.0) == 0.0
    assert eph.moon_shade(0.0, 0.0, 0.0) == pytest.approx(1.0)


def test_full_moon_is_nearly_uniform_to_the_limb():
    # lunar-Lambert (FMoonDisc::Shade): L(0) = 1 is Lommel-Seeliger, exactly uniform on the full disc
    assert eph.moon_lunar_lambert_l(0.0) == 1.0
    assert eph.moon_lunar_lambert_l(30.0) == pytest.approx(0.60838, abs=1e-5)
    assert eph.moon_shade(0.0, 0.95, 0.0) / eph.moon_shade(0.0, 0.0, 0.0) == pytest.approx(1.0)
    assert eph.moon_shade(0.95, 0.0, 5.0) / eph.moon_shade(0.0, 0.0, 5.0) > 0.75
