import pickle

import numpy as np
import shapely
from fake_sources import fake_context, source, synthetic_scene
from rasters import write_geotiff

from camsim_scene import datum, tiling
from camsim_scene.context import WorkerState, coverage
from camsim_scene.manifest import Manifest
from camsim_scene.sources import base


def test_query_orders_by_priority_and_footprint(tmp_path):
    st = WorkerState(fake_context(tmp_path))
    near = st.index["terrain"].query((10.1, 10.1, 10.2, 10.2))
    assert [e.qid for e in near] == ["hi_dem/hi_dem", "base_dem/base_dem"]
    assert [e.qid for e in st.index["terrain"].query((-50.0, -50.0, -49.0, -49.0))] == ["base_dem/base_dem"]
    assert near[0].order[:3] != near[1].order[:3]  # different merge groups


def test_coverage_uses_footprints_and_source_limits(tmp_path):
    m = Manifest.from_dict(fake_context(tmp_path).manifest)
    cov = coverage(m, "imagery")
    assert cov.limits == [8, 8] and cov.geoms[1].equals(shapely.box(*tiling.GLOBE))
    assert coverage(m, "terrain", use_bbox=True).geoms[0].equals(cov.geoms[0])


def test_context_pickles(tmp_path):
    ctx = fake_context(tmp_path)
    assert pickle.loads(pickle.dumps(ctx)) == ctx


def test_raster_geoid_datum_adds_the_geoid(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    geoid = write_geotiff(tmp_path / "geoid.tif", np.full((180, 360), 10.0, np.float32), -180.0, 90.0, 1.0)
    scene["sources"]["base_dem"] = source(
        "terrain",
        tmp_path / "src" / "base_dem.tif",
        [-180, -90, 180, 90],
        datum="wgs84_egm2008_raster",
        geoid=str(geoid),
        **{"global": True},
    )
    st = WorkerState(fake_context(tmp_path, scene))
    (e,) = st.index["terrain"].query((-50.0, -50.0, -49.0, -49.0))
    assert e.transform.vertical == "raster_geoid"
    assert np.allclose(e.transform.vertical_offset(np.array([-49.5]), np.array([-49.5])), 10.0)


class _ReadSpy:
    """Proxy for a rasterio dataset that records (overview level, window width, window height) of every read."""

    def __init__(self, ds, level, reads):
        self._ds, self._level, self._reads = ds, level, reads

    def __getattr__(self, name):
        return getattr(self._ds, name)

    def read(self, *args, window=None, **kw):
        self._reads.append((self._level, int(window.width), int(window.height)))
        return self._ds.read(*args, window=window, **kw)


def _geoid_state(tmp_path):
    """A 0.25-degree synthetic geoid (varying, with overviews down to 1/64) behind the global terrain source."""
    scene = synthetic_scene(tmp_path / "src")
    lon, lat = np.arange(1440) * 0.25 - 179.875, 89.875 - np.arange(720) * 0.25
    g = (30.0 * np.sin(np.radians(lat))[:, None] * np.cos(np.radians(2 * lon))[None, :]).astype(np.float32)
    geoid = write_geotiff(tmp_path / "geoid.tif", g, -180.0, 90.0, 0.25, overviews=(2, 4, 8, 16, 32, 64))
    scene["sources"]["base_dem"] = source(
        "terrain",
        tmp_path / "src" / "base_dem.tif",
        [-180, -90, 180, 90],
        datum="wgs84_egm2008_raster",
        geoid=str(geoid),
        **{"global": True},
    )
    (e,) = WorkerState(fake_context(tmp_path, scene)).index["terrain"].query((-50.0, -50.0, -49.0, -49.0))
    return e, str(geoid)


def _spy_reads(monkeypatch, geoid_path):
    reads = []
    real = base._dataset

    def spy(path, level):
        ds = real(path, level)
        return _ReadSpy(ds, level, reads) if path == geoid_path else ds

    monkeypatch.setattr(base, "_dataset", spy)
    return reads


def _tile_grid(z, x, y, margin=32 / 256):
    w, s, e, n = tiling.tile_bounds(z, x, y)
    pad = (e - w) * margin
    lon = np.linspace(max(w - pad, -180.0), min(e + pad, 180.0), 129)
    lat = np.linspace(max(s - pad, -90.0), min(n + pad, 90.0), 129)
    return np.meshgrid(lon, lat)


def test_coarse_geoid_queries_read_an_overview(tmp_path, monkeypatch):
    e, geoid = _geoid_state(tmp_path)
    for z, x, y, level in ((0, 0, 0, 3), (1, 1, 0, 2)):  # z0: 5.6-degree nodes -> 1/16; z1: 2.8 degrees -> 1/8
        reads = _spy_reads(monkeypatch, geoid)
        lon, lat = _tile_grid(z, x, y)
        datum.offset_lattice(e.transform, z, lon, lat)
        nodes = (np.ptp(lon) / (tiling.tile_size_deg(z) / 32) + 2) * (np.ptp(lat) / (tiling.tile_size_deg(z) / 32) + 2)
        assert reads and all(lv == level for lv, _, _ in reads), reads
        assert all(w * h <= 4 * nodes for _, w, h in reads), (reads, nodes)  # full resolution would be ~1000 x 800
    reads = _spy_reads(monkeypatch, geoid)
    datum.offset_lattice(e.transform, 12, *_tile_grid(12, 1000, 1000))
    assert reads and all(lv is None for lv, _, _ in reads)  # fine zooms keep the full-resolution grid


def test_coarse_geoid_neighbours_agree_on_their_edge(tmp_path):
    e, _ = _geoid_state(tmp_path)
    edge_lat = np.linspace(-80.0, 0.0, 57)
    edge = (np.zeros_like(edge_lat), edge_lat)  # the lon = 0 edge between z1 tiles (1, 0) and (2, 0)
    left = datum.offset_lattice(
        e.transform, 1, *[np.concatenate([v, g.ravel()]) for v, g in zip(edge, _tile_grid(1, 1, 0))]
    )
    right = datum.offset_lattice(
        e.transform, 1, *[np.concatenate([v, g.ravel()]) for v, g in zip(edge, _tile_grid(1, 2, 0))]
    )
    assert np.array_equal(left[: edge_lat.size], right[: edge_lat.size])
    assert np.ptp(left[: edge_lat.size]) > 1.0  # the geoid varies along the edge
