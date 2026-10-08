import math

import numpy as np
import pytest
from fake_sources import fake_context, source
from rasters import write_geotiff

from camsim_scene import qmesh, tiling
from camsim_scene.context import WorkerState
from camsim_scene.layers import terrain

GLOBE = [-180, -90, 180, 90]


def scene_with(tmp_path, hi_data, west, north, res, nodata=None, base_value=0.0):
    src = tmp_path / "src"
    src.mkdir(parents=True, exist_ok=True)
    write_geotiff(src / "base.tif", np.full((180, 360), base_value, np.float32), -180.0, 90.0, 1.0)
    write_geotiff(src / "hi.tif", hi_data.astype(np.float32), west, north, res, nodata=nodata, overviews=(2, 4, 8))
    h, w = hi_data.shape
    return {
        "name": "t",
        "bbox": [west, north - h * res, west + w * res, north],
        "priorities": {"terrain": ["hi", "base"], "imagery": [], "landcover": []},
        "sources": {
            "base": source("terrain", src / "base.tif", GLOBE, **{"global": True}),
            "hi": source(
                "terrain", src / "hi.tif", [west, north - h * res, west + w * res, north], max_zoom=16, nodata=nodata
            ),
        },
    }


def tin_on_grid(q, grid=257):
    """The decoded TIN (its own triangles) evaluated at the sample-grid nodes (row 0 = north)."""
    out = np.full((grid, grid), np.nan)
    gx, gy, h = q.u / qmesh.QMAX * (grid - 1), q.v / qmesh.QMAX * (grid - 1), q.heights()
    for a, b, c in q.triangles:
        xs, ys = gx[[a, b, c]], gy[[a, b, c]]
        X, Y = np.meshgrid(
            np.arange(int(np.floor(xs.min())), int(np.ceil(xs.max())) + 1),
            np.arange(int(np.floor(ys.min())), int(np.ceil(ys.max())) + 1),
        )
        det = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
        l1 = ((ys[1] - ys[2]) * (X - xs[2]) + (xs[2] - xs[1]) * (Y - ys[2])) / det
        l2 = ((ys[2] - ys[0]) * (X - xs[2]) + (xs[0] - xs[2]) * (Y - ys[2])) / det
        inside = (l1 >= -1e-3) & (l2 >= -1e-3) & (1 - l1 - l2 >= -1e-3) & (X >= 0) & (X < grid) & (Y >= 0) & (Y < grid)
        vals = l1 * h[a] + l2 * h[b] + (1 - l1 - l2) * h[c]
        out[grid - 1 - Y[inside], X[inside]] = vals[inside]
    return out


def tile_at(z, lon, lat):
    s = tiling.tile_size_deg(z)
    return math.floor((lon + 180) / s), math.floor((lat + 90) / s)


def f(lon, lat):
    return 100.0 + 30.0 * np.sin(lon * 600.0) * np.cos(lat * 450.0)


def smooth_scene(tmp_path):
    res, west, north, n = 0.00005, 9.99, 10.04, 1000  # covers the test tile and its margin
    lon = west + (np.arange(n) + 0.5) * res
    lat = north - (np.arange(n) + 0.5) * res
    return scene_with(tmp_path, f(lon[None, :], lat[:, None]), west, north, res)


def test_decoded_tin_is_within_max_error_of_the_grid_and_close_to_the_dem(tmp_path):
    st = WorkerState(fake_context(tmp_path, smooth_scene(tmp_path)))
    z = 13
    x, y = tile_at(z, 10.012, 10.012)
    entries = st.index["terrain"].query(terrain.query_bounds(z, x, y))
    b = tiling.tile_bounds(z, x, y)
    q = qmesh.decode(terrain.build_tile(z, x, y, entries))
    lon, lat = q.lonlat(b)
    h = q.heights()
    assert np.abs(h - f(lon, lat)).max() < 0.2  # vertices sit on samples of the DEM
    g = terrain.tile_grid(z, x, y, entries)
    assert np.nanmax(np.abs(tin_on_grid(q) - g.h)) <= terrain.max_error(z) + 0.05


def test_north_is_up(tmp_path):
    res, west, north, n = 0.00005, 9.99, 10.04, 1000
    lat = north - (np.arange(n) + 0.5) * res
    data = np.repeat((1000.0 * (lat - 9.99))[:, None], n, axis=1)  # rises northward
    st = WorkerState(fake_context(tmp_path, scene_with(tmp_path, data, west, north, res)))
    z = 13
    x, y = tile_at(z, 10.012, 10.012)
    q = qmesh.decode(terrain.build_tile(z, x, y, st.index["terrain"].query(terrain.query_bounds(z, x, y))))
    h = q.heights()
    assert h[q.north].min() > h[q.south].max()


def test_feather_is_continuous_across_a_shared_tile_edge(tmp_path):
    z = 15
    x, y = tile_at(z, 10.008, 10.008)
    e = tiling.tile_bounds(z, x, y)[2]
    res, west, north, n = 0.00002, 9.995, 10.02, 1250
    lon = west + (np.arange(n) + 0.5) * res
    data = np.where(lon[None, :] < e + 10.0 / 111320.0, 5.0, -9999.0) * np.ones((n, 1))
    st = WorkerState(fake_context(tmp_path, scene_with(tmp_path, data, west, north, res, nodata=-9999.0)))
    left = terrain.tile_grid(z, x, y, st.index["terrain"].query(terrain.query_bounds(z, x, y)))
    right = terrain.tile_grid(z, x + 1, y, st.index["terrain"].query(terrain.query_bounds(z, x + 1, y)))
    assert np.array_equal(left.h[:, -1], right.h[:, 0])
    row = np.concatenate([left.h[128], right.h[128, 1:]])
    assert row[0] == pytest.approx(5.0) and row[-1] == pytest.approx(0.0)
    assert np.all(np.diff(row) <= 1e-9)  # monotone ramp, no step
    assert left.seam.any() and right.seam.any()
    ql = qmesh.decode(terrain.build_tile(z, x, y, st.index["terrain"].query(terrain.query_bounds(z, x, y))))
    qr = qmesh.decode(terrain.build_tile(z, x + 1, y, st.index["terrain"].query(terrain.query_bounds(z, x + 1, y))))
    hl = dict(zip(ql.v[ql.east].tolist(), ql.heights()[ql.east]))
    hr = dict(zip(qr.v[qr.west].tolist(), qr.heights()[qr.west]))
    common = set(hl) & set(hr)
    assert len(common) >= 2
    step = max((ql.max_height - ql.min_height), (qr.max_height - qr.min_height)) / qmesh.QMAX
    assert all(abs(hl[v] - hr[v]) <= step + 1e-9 for v in common)


def test_lowest_priority_source_must_be_complete(tmp_path):
    scene = smooth_scene(tmp_path)
    scene["sources"]["base"]["global"] = False
    scene["sources"]["base"]["files"][0]["bbox"] = [0, 0, 1, 1]
    st = WorkerState(fake_context(tmp_path, scene))
    x, y = tile_at(13, 9.99, 10.012)  # straddles the DEM's west edge; the base no longer covers it
    with pytest.raises(terrain.TerrainError, match="gaps"):
        terrain.build_tile(13, x, y, st.index["terrain"].query(terrain.query_bounds(13, x, y)))


def test_point_heights_match_vertices_away_from_seams(tmp_path):
    st = WorkerState(fake_context(tmp_path, smooth_scene(tmp_path)))
    z = 13
    x, y = tile_at(z, 10.012, 10.012)
    entries = st.index["terrain"].query(terrain.query_bounds(z, x, y))
    q = qmesh.decode(terrain.build_tile(z, x, y, entries))
    lon, lat = q.lonlat(tiling.tile_bounds(z, x, y))
    assert np.abs(terrain.point_heights(z, lon, lat, entries) - q.heights()).max() < 0.2


def test_antimeridian_tile_gives_the_lattice_contiguous_longitudes(tmp_path, monkeypatch):
    seen = []
    real = terrain.offset_lattice

    def spy(dt, z, lon, lat):
        seen.append(np.asarray(lon).copy())
        return real(dt, z, lon, lat)

    monkeypatch.setattr(terrain, "offset_lattice", spy)
    st = WorkerState(fake_context(tmp_path, smooth_scene(tmp_path)))
    z = 13
    for x in (0, (1 << (z + 1)) - 1):
        entries = st.index["terrain"].query(terrain.query_bounds(z, x, 4000))
        g = terrain.tile_grid(z, x, 4000, entries)
        assert np.isfinite(g.h).all()
        lon = seen[-1]
        assert np.ptp(lon) < 2 * tiling.tile_size_deg(z)  # unwrapped, not spanning -180..180
        assert lon.min() < -180.0 or lon.max() > 180.0
        assert terrain.sample_grid(z, x, 4000)[0].min() >= -180.0  # rasters still see wrapped longitudes
