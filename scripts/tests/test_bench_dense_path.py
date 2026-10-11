"""dense_pass path selection: land-cover reading, strip scoring, ranking (REALISM R3/R4 bench)."""

import pytest
import numpy as np
from PIL import Image

from bench import dense_path as dp


def _tile(root, i, j, fill):
    a = np.full((600, 600), fill, np.uint8)
    Image.fromarray(a).save(root / dp.lc_tile_filename(i, j))
    return a


def test_tile_filename_matches_the_landcover_layer():
    assert dp.lc_tile_filename(665, -2349) == "+33.25_-117.45.png"


def test_landcover_reads_codes_and_missing_tiles_are_zero(tmp_path):
    a = _tile(tmp_path, 665, -2349, 30)
    a[0, 0] = 10  # north-west corner texel
    Image.fromarray(a).save(tmp_path / dp.lc_tile_filename(665, -2349))
    lc = dp.LandCover(tmp_path)
    got = lc.codes(np.array([-117.45 + 1e-6, -117.40 + 0.01]), np.array([33.30 - 1e-6, 33.27]))
    assert got.tolist() == [10, 0]


def test_landcover_tile_edge_uses_the_rounded_tile_index(tmp_path):
    # 33.3 / 0.05 = 665.9999999999999 in floats: without round(x, 9) the point on the tile's south edge would read the
    # tile below (south-west 33.25), as the scene tooling's tile index rounds it into the tile above.
    assert 33.3 / dp.TILE_DEG < 666
    below = _tile(tmp_path, 665, -2349, 30)
    above = _tile(tmp_path, 666, -2349, 10)
    del below, above
    got = dp.LandCover(tmp_path).codes(np.array([-117.44]), np.array([33.3]))
    assert got.tolist() == [10]


def test_class_grid_fractions(tmp_path):
    a = np.full((600, 600), 30, np.uint8)
    a[:, :300] = 10  # west half trees
    Image.fromarray(a).save(tmp_path / dp.lc_tile_filename(665, -2349))
    g = dp.class_grid(dp.LandCover(tmp_path), (-117.45, 33.25, -117.40, 33.30), cell_m=500.0)
    assert g.tree.shape == g.built.shape
    assert g.tree[:, 0].min() == 1.0 and g.tree[:, -1].max() == 0.0
    assert g.built.max() == 0.0


def test_strip_points_run_along_the_bearing():
    x, y = dp.strip_points(0.0, 0.0, 90.0, 1000.0, near_m=0.0, far_m=0.0, half_width_m=0.0, step_m=500.0)
    assert np.allclose(sorted(set(np.round(x))), [0.0, 500.0, 1000.0])
    assert np.allclose(y, 0.0, atol=1e-9)


def _synthetic_grid(n=120, cell=100.0):
    tree = np.zeros((n, n))
    built = np.zeros((n, n))
    tree[60:62, :] = 1.0  # an east-west tree band (row index runs north to south)
    built[:, 30:32] = 1.0  # a north-south built band
    return dp.Grid(-117.5, 33.2, cell, tree, built)


def test_best_candidate_needs_both_classes_and_stays_inside():
    g = _synthetic_grid()
    ranked = dp.candidates(g, length_m=4000.0, start_step_m=1000.0, bearing_step_deg=45.0)
    best = ranked[0]
    assert best.tree > 0 and best.built > 0
    assert all(c.tree > 0 and c.built > 0 for c in ranked)
    ext = g.tree.shape[1] * g.cell_m
    for c in ranked:
        x1 = c.x0 + 4000.0 * np.sin(np.radians(c.bearing_deg))
        y1 = c.y0 + 4000.0 * np.cos(np.radians(c.bearing_deg))
        assert 500.0 <= min(c.x0, x1) and max(c.x0, x1) <= ext - 500.0
        assert 500.0 <= min(c.y0, y1) and max(c.y0, y1) <= ext - 500.0
    assert ranked == sorted(ranked, key=lambda c: -c.score)


def test_ranking_is_deterministic():
    g = _synthetic_grid()
    a = dp.candidates(g, length_m=4000.0, bearing_step_deg=45.0)
    b = dp.candidates(g, length_m=4000.0, bearing_step_deg=45.0)
    assert a == b


def test_xy_to_lonlat_round_trip_origin():
    g = _synthetic_grid()
    lon, lat = dp.xy_to_lonlat(g, 0.0, 0.0)
    assert (lon, lat) == (-117.5, 33.2)
    lon, lat = dp.xy_to_lonlat(g, 0.0, dp.M_PER_DEG)
    assert abs(lat - 34.2) < 1e-9


def _qmesh_package(tmp_path, height):
    qmesh = pytest.importorskip("camsim_scene.qmesh")
    z, d = 12, 180.0 / 2**12
    lon, lat = -117.45, 33.30
    x, y = int((lon + 180.0) // d), int((lat + 90.0) // d)
    w, s = -180.0 + x * d, -90.0 + y * d
    u = np.array([w, w + d, w, w + d])
    v = np.array([s, s, s + d, s + d])
    h = np.full(4, height)
    raw = qmesh.encode(u, v, h, np.array([[0, 1, 2], [1, 3, 2]]), (w, s, w + d, s + d))
    p = tmp_path / "terrain" / str(z) / str(x)
    p.mkdir(parents=True)
    qmesh.write_tile(p / f"{y}.terrain", raw)  # write_tile gzips; do not gzip_tile first (it would be gzipped twice)
    return tmp_path, lon, lat


def test_terrain_max_height_reads_the_package_tile(tmp_path):
    from bench import select_dense_path as sdp

    pkg, lon, lat = _qmesh_package(tmp_path, 123.0)
    # The tile is ~4.9 km wide, so a 250 m radius may contain no vertex: the reader falls back to the
    # nearest vertex rather than answering None.
    assert abs(sdp.terrain_max_height(pkg, lon, lat) - 123.0) < 0.5


def test_terrain_max_height_missing_tile_is_none(tmp_path):
    pytest.importorskip("camsim_scene.qmesh")
    from bench import select_dense_path as sdp

    assert sdp.terrain_max_height(tmp_path, -117.45, 33.30) is None
