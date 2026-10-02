import math

import pytest

from camsim_hitl.frames import q_nb_from_euler_deg
from camsim_hitl.geodesy import (
    Geoid,
    TerrainBlend,
    apply_lever_arm,
    ned_distance,
    offset_ned,
    radii,
)


@pytest.fixture(scope="module")
def geoid():
    g = Geoid()
    if not g.available:
        pytest.skip("EGM96 grid not available (git lfs pull)")
    return g


@pytest.mark.parametrize(
    "lat,lon,n",
    [
        (90.0, 0.0, 13.61),  # published EGM96 grid nodes (cm resolution)
        (-90.0, 0.0, -29.53),
        (0.0, 0.0, 17.16),
    ],
)
def test_geoid_grid_nodes(geoid, lat, lon, n):
    assert geoid.undulation(lat, lon) == pytest.approx(n, abs=0.005)


def test_geoid_extremes_and_us(geoid):
    # Indian Ocean low (about -107 m) and New Guinea high (about +85 m)
    assert geoid.undulation(4.7, 78.8) == pytest.approx(-107, abs=1.0)
    assert geoid.undulation(-8.0, 147.0) == pytest.approx(84, abs=2.0)
    # San Francisco: about -32 m (continental US: -8 to -50)
    assert -35 < geoid.undulation(37.7749, -122.4194) < -30
    # longitude wrap: -122 == 238
    assert geoid.undulation(37.0, -122.0) == pytest.approx(geoid.undulation(37.0, 238.0))


def test_msl_ellipsoid_round_trip(geoid):
    h = geoid.msl_to_ellipsoid(37.6, -122.4, 100.0)
    assert h == pytest.approx(100.0 + geoid.undulation(37.6, -122.4))
    assert geoid.ellipsoid_to_msl(37.6, -122.4, h) == pytest.approx(100.0)


def test_missing_geoid(tmp_path):
    with pytest.raises(FileNotFoundError):
        Geoid(tmp_path / "nope.DAC")
    bad = tmp_path / "pointer.DAC"
    bad.write_text("version https://git-lfs.github.com/spec/v1\n")
    with pytest.raises(ValueError):
        Geoid(bad)
    g = Geoid(bad, allow_missing=True)
    assert g.undulation(10, 10) == 0.0


def test_radii():
    m, n = radii(0.0)
    assert n == pytest.approx(6378137.0)
    assert m == pytest.approx(6335439.327, abs=1e-3)
    m, n = radii(90.0)
    assert m == pytest.approx(n)
    assert n == pytest.approx(6399593.626, abs=1e-3)


def test_offset_ned_one_km():
    lat, lon, h = offset_ned(45.0, 10.0, 100.0, 1000.0, 1000.0, -50.0)
    assert ned_distance(45.0, 10.0, lat, 10.0) == pytest.approx(1000.0, rel=1e-4)
    assert ned_distance(45.0, 10.0, 45.0, lon) == pytest.approx(1000.0, rel=1e-3)
    assert h == pytest.approx(150.0)


def test_lever_arm_rotates_with_attitude():
    r = (2.0, 0.0, 1.0)  # 2 m forward, 1 m below the reference point
    lat, lon, h = apply_lever_arm(0.0, 0.0, 0.0, q_nb_from_euler_deg(0, 0, 0), r)
    m, n = radii(0.0)
    assert math.radians(lat) * m == pytest.approx(2.0, abs=1e-6)
    assert lon == pytest.approx(0.0, abs=1e-12)
    assert h == pytest.approx(-1.0)
    # heading east: forward offset goes east
    lat, lon, h = apply_lever_arm(0.0, 0.0, 0.0, q_nb_from_euler_deg(90, 0, 0), r)
    assert math.radians(lon) * n == pytest.approx(2.0, abs=1e-6)
    assert lat == pytest.approx(0.0, abs=1e-12)
    # pitched 90 up: forward is up
    lat, lon, h = apply_lever_arm(0.0, 0.0, 0.0, q_nb_from_euler_deg(0, 90, 0), r)
    assert h == pytest.approx(2.0, abs=1e-9)


def test_terrain_blend_band_and_smoothing():
    b = TerrainBlend(band_low_m=50, band_high_m=100, tau_s=0.0)
    assert b.weight(10) == 1.0 and b.weight(200) == 0.0 and 0 < b.weight(75) < 1
    # no HOT yet: plain ellipsoid altitude
    assert b.altitude(37.0, -122.0, 20.0, 5.0, 0.0) == 20.0
    # Cesium ground at 10 m ellipsoid, wheels 5 m up -> 15 m, regardless of X-Plane's 20
    b.update_hot(37.0, -122.0, 10.0, 0.0)
    assert b.altitude(37.0, -122.0, 20.0, 5.0, 0.1) == pytest.approx(15.0)
    # above the band the correction no longer applies
    assert b.altitude(37.0, -122.0, 300.0, 250.0, 0.2) == pytest.approx(300.0)
    # stale or far HOT is ignored
    assert b.altitude(37.01, -122.0, 20.0, 5.0, 0.3) == pytest.approx(20.0)
    b.update_hot(37.0, -122.0, 10.0, 0.0)
    assert b.altitude(37.0, -122.0, 20.0, 5.0, 10.0) == pytest.approx(20.0)


def test_terrain_blend_low_pass():
    b = TerrainBlend(tau_s=0.5)
    b.altitude(37.0, -122.0, 20.0, 5.0, 0.0)  # starts with no correction
    b.update_hot(37.0, -122.0, 10.0, 0.0)
    a1 = b.altitude(37.0, -122.0, 20.0, 5.0, 0.1)
    assert 15.0 < a1 < 20.0  # moving smoothly toward 15
    a2 = b.altitude(37.0, -122.0, 20.0, 5.0, 3.0)
    assert a2 == pytest.approx(15.0, abs=0.05)
