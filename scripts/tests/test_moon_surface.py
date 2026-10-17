"""scripts/sky/moon_surface.py: the Python mirror of the dome's moon disc (Environment/MoonSurface.cpp, CamSimMoonDisc)
used by night_check.py N3, held to the same references as the C++ tests (CamSim.Environment.MoonSurface.*)."""

import math
import sys
from pathlib import Path

import pytest

np = pytest.importorskip("numpy")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sky"))
import moon_surface as ms  # noqa: E402


def test_iau_orientation_matches_meeus_example_53a():
    # Meeus ex. 53.a (1992-04-12 0h TD, the moon of ex. 47.a): l = -1.23, b = +4.20, P = 15.08 deg
    d = 2448724.5 - 2451545.0
    ra, dec = math.radians(134.688470), math.radians(13.768368)
    moon_date = np.array([math.cos(dec) * math.cos(ra), math.cos(dec) * math.sin(ra), math.sin(dec)])
    moon = ms.precession_j2000_to_date(d / 36525.0).T @ moon_date
    axes = ms.iau_axes_j2000(d)
    b = axes @ -moon
    assert math.degrees(math.atan2(b[1], b[0])) == pytest.approx(-1.23, abs=0.15)
    assert math.degrees(math.asin(b[2])) == pytest.approx(4.20, abs=0.15)
    raj, decj = math.atan2(moon[1], moon[0]), math.asin(moon[2])
    north = np.array([-math.sin(decj) * math.cos(raj), -math.sin(decj) * math.sin(raj), math.cos(decj)])
    east = np.array([-math.sin(raj), math.cos(raj), 0.0])
    assert math.degrees(math.atan2(axes[2] @ east, axes[2] @ north)) == pytest.approx(15.08, abs=0.3)


def test_albedo_sampling_wraps_and_clamps():
    img = np.array([[0, 51, 102, 255], [0, 51, 102, 255]], dtype=float) / 255.0
    lat0 = (0.5 - 0.5 / 2.0) * math.pi
    lon1 = (1.5 / 4.0 - 0.5) * 2.0 * math.pi
    assert ms.sample_albedo(img, np.array([lon1]), np.array([lat0]))[0] == pytest.approx(0.2)
    assert ms.sample_albedo(img, np.array([math.pi]), np.array([lat0]))[0] == pytest.approx(0.5)
    assert ms.sample_albedo(img, np.array([lon1]), np.array([math.pi / 2]))[0] == pytest.approx(0.2)
    assert ms.sample_albedo(None, np.array([0.3]), np.array([0.1]))[0] == 1.0


def test_full_disc_is_uniform_without_albedo_and_lit_toward_the_sun():
    moon = np.array([1.0, 0.0, 0.0])
    axes = np.array([[-1.0, 0.0, 0.0], [0.0, -1.0, 0.0], [0.0, 0.0, 1.0]])
    r = math.radians(0.26)
    t = np.linspace(-0.95, 0.95, 39)
    view = np.stack([np.ones_like(t), t * math.sin(r), np.zeros_like(t)], axis=-1)
    view /= np.linalg.norm(view, axis=-1, keepdims=True)
    full = ms.disc_radiance(view, moon, np.array([-1.0, 1e-9, 0.0]), r, ms.lunar_lambert_l(0.0), axes, None)
    assert full.min() == pytest.approx(1.0, abs=1e-6) and full.max() == pytest.approx(1.0, abs=1e-6)
    a = math.radians(90.0)   # quarter, sun toward +y on the sky
    quarter = ms.disc_radiance(view, moon, np.array([-math.cos(a), math.sin(a), 0.0]), r, ms.lunar_lambert_l(90.0), axes, None)
    assert quarter[t > 0.1].min() > 0.0 and quarter[t < -0.1].max() == 0.0


def test_shipped_albedo_map_loads_with_dark_maria():
    img = ms.load_albedo()
    if img is None:
        pytest.skip("moon_albedo.pgm not present (git lfs pull)")
    assert img.shape == (512, 1024)
    crisium = ms.sample_albedo(img, np.radians([59.0]), np.radians([17.0]))[0]
    highlands = ms.sample_albedo(img, np.radians([20.0]), np.radians([-20.0]))[0]
    assert crisium < 0.6 * highlands
