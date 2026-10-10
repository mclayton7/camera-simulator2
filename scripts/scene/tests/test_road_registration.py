"""road_registration.py: the dark-box profile match, the junction set and the TMS sampler on synthetic data."""

from __future__ import annotations

import importlib.util
from pathlib import Path

import numpy as np
from PIL import Image
from shapely.geometry import LineString

from camsim_scene.features import Feature

_SPEC = importlib.util.spec_from_file_location(
    "road_registration", Path(__file__).resolve().parents[1] / "tools" / "road_registration.py"
)
rr = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(rr)
road_offset = rr.road_offset
X = np.arange(-30.0, 30.01, 0.5)


def test_road_offset_finds_a_dark_band():
    prof = np.where(np.abs(X - 4.0) <= 5.0, 60.0, 160.0)
    assert abs(road_offset(prof, 0.5, 10.0) - 4.0) <= 0.5
    assert rr.road_match(prof, 0.5, 10.0)["polarity"] == "dark"


def test_road_offset_finds_a_bright_band():
    prof = np.where(np.abs(X + 6.0) <= 5.0, 200.0, 120.0)  # bright asphalt/concrete against dark chaparral
    assert abs(road_offset(prof, 0.5, 10.0) + 6.0) <= 0.5
    assert rr.road_match(prof, 0.5, 10.0)["polarity"] == "bright"


def test_road_offset_none_on_a_flat_profile():
    assert road_offset(np.full(121, 120.0), 0.5, 10.0) is None


def test_road_offset_none_on_a_single_edge():
    assert road_offset(np.where(X < 3.0, 80.0, 160.0), 0.5, 10.0) is None  # one edge, no pair


def test_road_offset_none_on_weak_edges():
    assert road_offset(np.where(np.abs(X) <= 5.0, 117.0, 120.0), 0.5, 10.0) is None  # 3 DN: below EDGE_MIN


def test_road_offset_tolerates_width_error():
    prof = np.where(np.abs(X - 4.0) <= 5.0, 60.0, 160.0)  # a 10 m road
    for w in (8.0, 12.0):
        assert abs(road_offset(prof, 0.5, w) - 4.0) <= 1.0


def test_road_offset_ignores_noise_on_a_clear_road():
    rng = np.random.default_rng(0)
    prof = np.where(np.abs(X - 2.0) <= 4.0, 180.0, 110.0) + rng.normal(0.0, 4.0, X.size)
    assert abs(road_offset(prof, 0.5, 8.0) - 2.0) <= 1.0


def _road(oid, coords, **tags):
    return Feature("road", f"osm:w{oid}", LineString(coords), {"highway": "primary", **tags})


def test_junctions_are_nodes_shared_by_two_ways():
    a = _road(1, [(0.0, 0.0), (0.001, 0.0), (0.002, 0.0)])
    b = _road(2, [(0.001, 0.0), (0.001, 0.001)])
    c = _road(3, [(0.005, 0.005), (0.006, 0.005)])
    assert rr.junctions([a, b, c]) == [(0.001, 0.0)]


def test_candidate_ways_keep_dark_asphalt_main_roads():
    keep = _road(1, [(0, 0), (0.001, 0)])
    link = _road(2, [(0, 0), (0.001, 0)], highway="primary_link")
    resid = _road(3, [(0, 0), (0.001, 0)], highway="residential")
    concrete = _road(4, [(0, 0), (0.001, 0)], surface="concrete")
    bridge = _road(5, [(0, 0), (0.001, 0)], bridge="yes")
    kept, skipped = rr.split_ways([keep, link, resid, concrete, bridge])
    assert [f.source_id for f in kept] == ["osm:w1"]
    assert [f.source_id for f in skipped] == ["osm:w5"]


def test_sampler_reads_bilinear_luma(tmp_path):
    z = 17
    t = 180.0 / (1 << z)
    x, y = 45400, 89836  # a tile: lon -180 + x t, lat -90 + y t (south edge)
    (tmp_path / str(z) / str(x)).mkdir(parents=True)
    img = np.zeros((256, 256), np.uint8)
    img[:, 128:] = 200  # east half bright
    Image.fromarray(img).convert("RGB").save(tmp_path / str(z) / str(x) / f"{y}.jpg", quality=100)
    s = rr.TmsSampler(tmp_path, z)
    lat = -90.0 + (y + 0.5) * t
    lon_w, lon_e = -180.0 + (x + 0.25) * t, -180.0 + (x + 0.75) * t
    v = s.luma(np.array([lon_w, lon_e]), np.array([lat, lat]))
    assert v[0] < 10 and v[1] > 190
    assert np.isnan(s.luma(np.array([lon_w + 10 * t]), np.array([lat]))[0])  # no tile: NaN
