"""road_registration.py: window selection, the boundary raster, the 2-D shift and its prominence rule, and the TMS
sampler, on synthetic data."""

from __future__ import annotations

import importlib.util
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter
from shapely import affinity
from shapely.geometry import LineString

from camsim_scene.features import Feature

_SPEC = importlib.util.spec_from_file_location(
    "road_registration", Path(__file__).resolve().parents[1] / "tools" / "road_registration.py"
)
rr = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(rr)

H, S = 200.0, 20.0  # a small window for speed: half-size and search radius, metres
PATTERN = [  # (local LineString, width m): a crossroads with a curve
    (LineString([(-200, -40), (200, -40)]), 10.0),
    (LineString([(30, -200), (30, 200)]), 7.0),
    (LineString([(-200, 120), (-60, 150), (60, 60), (200, 90)]), 8.0),
]


def _road(oid, coords, **tags):
    return Feature("road", f"osm:w{oid}", LineString(coords), {"highway": "primary", **tags})


def test_road_ways_keep_paved_main_roads_and_links():
    fs = [
        _road(1, [(0, 0), (0.001, 0)]),
        _road(2, [(0, 0), (0.001, 0)], highway="residential_link"),
        _road(3, [(0, 0), (0.001, 0)], highway="service"),
        _road(4, [(0, 0), (0.001, 0)], surface="concrete"),
        _road(5, [(0, 0), (0.001, 0)], surface="gravel"),
        _road(6, [(0, 0), (0.001, 0)], bridge="yes"),
    ]
    assert [f.source_id for f in rr.road_ways(fs)] == ["osm:w1", "osm:w2", "osm:w4"]


def test_windows_need_the_road_length_minimum():
    lat, lon = 33.3, -117.4
    dlat, dlon = 1000.0 / rr.M_PER_DEG_LAT, 1000.0 / (rr.M_PER_DEG_LAT * np.cos(np.radians(lat)))
    bbox = (lon, lat, lon + 2 * dlon, lat + dlat)  # two windows side by side
    long_road = _road(1, [(lon + 0.05 * dlon, lat + 0.5 * dlat), (lon + 0.95 * dlon, lat + 0.5 * dlat)])  # 900 m
    more = _road(2, [(lon + 0.5 * dlon, lat + 0.05 * dlat), (lon + 0.5 * dlon, lat + 0.95 * dlat)])  # +900 m
    short = _road(3, [(lon + 1.2 * dlon, lat + 0.5 * dlat), (lon + 1.8 * dlon, lat + 0.5 * dlat)])  # 600 m
    wins = rr.qualifying_windows([long_road, more, short], bbox, 0, 60)
    assert [(w["i"], w["j"]) for w in wins] == [(0, 0)]
    assert abs(wins[0]["road_m"] - 1800.0) < 20.0


def test_window_selection_is_capped_and_seeded():
    lat, lon = 33.3, -117.4
    dlat, dlon = 1000.0 / rr.M_PER_DEG_LAT, 1000.0 / (rr.M_PER_DEG_LAT * np.cos(np.radians(lat)))
    bbox = (lon, lat, lon + 6 * dlon, lat + dlat)
    roads = [
        _road(k, [(lon + (k + 0.02) * dlon, lat + y * dlat), (lon + (k + 0.98) * dlon, lat + y * dlat)])
        for k in range(6)
        for y in (0.3, 0.7)
    ]
    a = rr.qualifying_windows(roads, bbox, 7, 3)
    assert len(a) == 3 and a == rr.qualifying_windows(roads, bbox, 7, 3)


def test_road_masks_draw_both_edges_and_the_interior():
    b, interior = rr.road_masks([(LineString([(-50, 0), (50, 0)]), 10.0)], (-50, -50, 50, 50), 1.0)
    assert set(np.flatnonzero(interior[:, 50])) == set(
        range(46, 54)
    )  # pixel centres |y| <= 3.5: the 5 m half-width inset 1.5 m
    col = b[:, 50]
    rows = np.flatnonzero(col)
    assert set(rows) <= set(range(43, 47)) | set(range(53, 57))  # y = +5 is row 45, y = -5 is row 55
    assert col[44] or col[45]
    assert col[54] or col[55]
    assert not col[50]  # the road's interior is not an edge


def test_window_shift_recovers_a_known_shift():
    b, interior = rr.road_masks(PATTERN, (-H, -H, H, H), 1.0)
    shifted = [(affinity.translate(g, 4.0, -3.0), w) for g, w in PATTERN]
    edges = rr.road_masks(shifted, (-H - S, -H - S, H + S, H + S), 1.0)[0].astype(np.float64)
    g = gaussian_filter(edges, 1.0) * 40.0
    g[:, 300:302] += 15.0  # a decoy: one long straight edge (a field boundary)
    g[100:140, 100:300] += np.random.default_rng(2).uniform(0.0, 30.0, (40, 200))  # a textured block (houses, trees)
    g += np.random.default_rng(0).normal(0.0, 2.0, g.shape)
    r = rr.window_shift(np.abs(g), b, interior, 1.0, S)
    assert r["used"]
    assert abs(r["dE"] - 4.0) <= 1.0 and abs(r["dN"] + 3.0) <= 1.0


def test_featureless_window_is_excluded():
    b, interior = rr.road_masks(PATTERN, (-H, -H, H, H), 1.0)
    noise = np.random.default_rng(1).normal(0.0, 1.0, (int(2 * (H + S)),) * 2)
    g = rr.gradient_magnitude(120.0 + noise, 1.0)
    assert not rr.window_shift(g, b, interior, 1.0, S)["used"]


def test_sampler_reads_bilinear_luma(tmp_path):
    z = 17
    t = 180.0 / (1 << z)
    x, y = 45400, 89836  # a tile: lon -180 + x t, lat -90 + y t (south edge)
    (tmp_path / str(z) / str(x)).mkdir(parents=True)
    img = np.zeros((256, 256), np.uint8)
    img[:, 128:] = 200  # east half bright
    img[:128, :128] = 100  # north-west quarter mid grey (row 0 is north)
    Image.fromarray(img).convert("RGB").save(tmp_path / str(z) / str(x) / f"{y}.jpg", quality=100)
    s = rr.TmsSampler(tmp_path, z)
    lat_s, lat_n = -90.0 + (y + 0.25) * t, -90.0 + (y + 0.75) * t
    lon_w, lon_e = -180.0 + (x + 0.25) * t, -180.0 + (x + 0.75) * t
    v = s.luma(np.array([lon_w, lon_e, lon_w]), np.array([lat_s, lat_s, lat_n]))
    assert v[0] < 10 and v[1] > 190 and 90 < v[2] < 110
    assert np.isnan(s.luma(np.array([lon_w + 10 * t]), np.array([lat_s]))[0])  # no tile: NaN
