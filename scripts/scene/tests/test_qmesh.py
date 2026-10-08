import gzip
import io

import numpy as np
import pytest

from camsim_scene import qmesh
from camsim_scene.qmesh import QMAX


def grid_mesh(bounds, n=9, hfun=lambda lon, lat: 100.0 + 1000.0 * (lon - lon.min()) - 300.0 * (lat - lat.min())):
    """Regular n x n vertex grid in two CCW triangles per cell (x east, y north); triangle order scrambled."""
    w, s, e, nn = bounds
    c, r = np.meshgrid(np.arange(n), np.arange(n))
    lon = w + c.ravel() * (e - w) / (n - 1)
    lat = s + r.ravel() * (nn - s) / (n - 1)
    tris = []
    for rr in range(n - 1):
        for cc in range(n - 1):
            a, b, d, f = rr * n + cc, rr * n + cc + 1, (rr + 1) * n + cc + 1, (rr + 1) * n + cc
            tris += [(a, b, d), (a, d, f)]
    tris = np.array(tris)[np.random.default_rng(1).permutation(len(tris))]
    return lon, lat, hfun(lon, lat), tris


B16 = (-117.388916015625, 33.2171630859375, -117.38616943359375, 33.21990966796875)  # a z16 tile


def test_round_trip_positions_heights_and_triangles():
    lon, lat, h, tris = grid_mesh(B16)
    q = qmesh.decode(qmesh.encode(lon, lat, h, tris, B16))
    order, t2 = qmesh.renumber(tris)
    assert np.array_equal(q.triangles, t2)
    dlon, dlat = q.lonlat(B16)
    assert np.abs(dlon - lon[order]).max() <= 0.5 * (B16[2] - B16[0]) / QMAX + 1e-12
    assert np.abs(dlat - lat[order]).max() <= 0.5 * (B16[3] - B16[1]) / QMAX + 1e-12
    assert np.abs(q.heights() - h[order]).max() <= 0.5 * (h.max() - h.min()) / QMAX + 1e-4
    assert q.min_height <= h.min() and q.max_height >= h.max()


def test_high_water_mark_codes_round_trip_and_are_valid():
    _, t2 = qmesh.renumber(grid_mesh(B16)[3])
    flat = t2.ravel()
    codes = qmesh.hwm_encode(flat)
    assert np.array_equal(qmesh.hwm_decode(codes), flat)
    running = np.maximum.accumulate(np.concatenate([[-1], flat[:-1]]))
    assert np.all(flat <= running + 1)  # every index is old or exactly the next new one


def test_edges_are_complete_exact_and_sorted():
    lon, lat, h, tris = grid_mesh(B16, n=9)
    q = qmesh.decode(qmesh.encode(lon, lat, h, tris, B16))
    for idx, coord, other, value in (
        (q.west, q.u, q.v, 0),
        (q.east, q.u, q.v, QMAX),
        (q.south, q.v, q.u, 0),
        (q.north, q.v, q.u, QMAX),
    ):
        assert len(idx) == 9
        assert np.all(coord[idx] == value)
        assert np.all(np.diff(other[idx].astype(int)) > 0)
    assert (q.u == 0).sum() == 9 and (q.u == QMAX).sum() == 9


def test_triangles_are_counter_clockwise():
    lon, lat, h, tris = grid_mesh(B16)
    q = qmesh.decode(qmesh.encode(lon, lat, h, tris, B16))
    u, v = q.u.astype(float), q.v.astype(float)
    a, b, c = q.triangles.T
    area2 = (u[b] - u[a]) * (v[c] - v[a]) - (u[c] - u[a]) * (v[b] - v[a])
    assert np.all(area2 > 0)


def test_normals_finite_unit_and_outward_even_at_the_poles():
    b = (-180.0, -90.0, 0.0, 90.0)  # z0 west tile: rows at lat +-90 make degenerate triangles
    lon, lat, h, tris = grid_mesh(b, n=5, hfun=lambda lon, lat: np.zeros_like(lon))
    q = qmesh.decode(qmesh.encode(lon, lat, h, tris, b))
    n = q.normals()
    assert np.all(np.isfinite(n)) and np.allclose(np.linalg.norm(n, axis=1), 1.0, atol=1e-6)
    dlon, dlat = q.lonlat(b)
    assert np.all((n * qmesh.geodetic_up(dlon, dlat)).sum(1) > 0.9)
    assert np.all(np.isfinite(q.horizon_occlusion)) and np.isfinite(q.sphere_radius)


def test_bytes_are_identical_across_runs_and_gzip_has_no_timestamp():
    lon, lat, h, tris = grid_mesh(B16)
    a = qmesh.gzip_tile(qmesh.encode(lon, lat, h, tris, B16))
    b = qmesh.gzip_tile(qmesh.encode(lon, lat, h, tris, B16))
    assert a == b and a[4:8] == b"\0\0\0\0"
    assert qmesh.decode(a).triangles.shape == qmesh.decode(gzip.decompress(a)).triangles.shape


def test_more_than_65536_vertices_uses_32_bit_indices():
    lon, lat, h, tris = grid_mesh(B16, n=258)
    raw = qmesh.encode(lon, lat, h, tris, B16)
    q = qmesh.decode(raw)
    assert len(q.u) == 258 * 258 and q.triangles.max() == 258 * 258 - 1
    assert len(q.west) == 258


def test_decoder_reads_an_independent_encoder():
    qme = pytest.importorskip("quantized_mesh_encoder")
    b = (0.0, 0.0, 1.0, 1.0)
    lon, lat, h, tris = grid_mesh(b, n=5)
    order, t2 = qmesh.renumber(tris)  # quantized-mesh-encoder doesn't reorder
    pos = np.stack([lon[order], lat[order], h[order]], 1).astype(np.float32)
    buf = io.BytesIO()
    qme.encode(buf, pos, t2.astype(np.uint32), bounds=b)
    q = qmesh.decode(buf.getvalue())
    assert np.array_equal(q.triangles, t2)
    assert np.abs(q.u.astype(float) - lon[order] * QMAX).max() <= 1.0  # it truncates; we round
    assert np.abs(q.v.astype(float) - lat[order] * QMAX).max() <= 1.0
    assert np.abs(q.heights() - h[order]).max() <= (h.max() - h.min()) / QMAX * 1.01 + 1e-3
    assert len(q.west) == len(q.east) == len(q.south) == len(q.north) == 5


def test_layer_json_shape():
    lj = qmesh.layer_json("pendleton", [[{"startX": 0, "startY": 0, "endX": 1, "endY": 0}]], "USGS")
    assert lj["format"] == "quantized-mesh-1.0" and lj["scheme"] == "tms" and lj["projection"] == "EPSG:4326"
    assert lj["extensions"] == ["octvertexnormals"] and lj["maxzoom"] == 0 and lj["tiles"] == ["{z}/{x}/{y}.terrain"]


def test_exactly_65536_vertices_round_trip():
    """65,536 vertices: a high-water-mark code can reach 65,536, which uint16 indices can't hold (Cesium switches
    to 32-bit only above 65,536 vertices). The last triangle repeats the first, so it references vertex 0 when
    every vertex has been seen."""
    n = 256
    w, s, e, nn = B16
    c, r = np.meshgrid(np.arange(n), np.arange(n))
    lon = w + c.ravel() * (e - w) / (n - 1)
    lat = s + r.ravel() * (nn - s) / (n - 1)
    h = 100.0 + 50.0 * np.sin(c.ravel() / 9.0) * np.cos(r.ravel() / 7.0)
    a = (r[:-1, :-1] * n + c[:-1, :-1]).ravel()
    tris = np.concatenate([np.stack([a, a + 1, a + n + 1], 1), np.stack([a, a + n + 1, a + n], 1)])
    tris = np.concatenate([tris, tris[:1]])
    q = qmesh.decode(qmesh.encode(lon, lat, h, tris, B16))
    order, t2 = qmesh.renumber(tris)
    assert len(order) == 65536
    assert np.array_equal(q.triangles, t2)
    assert np.abs(q.heights()[: len(order)] - h[order]).max() <= 0.5 * (h.max() - h.min()) / QMAX + 1e-4
    for edge, along in ((q.west, q.v), (q.south, q.u), (q.east, q.v), (q.north, q.u)):
        assert len(edge) == n and edge.max() < len(order) and np.all(np.diff(along[edge].astype(int)) > 0)


def test_index_codes_that_overflow_their_type_raise(monkeypatch):
    lon, lat, h, tris = grid_mesh(B16)
    monkeypatch.setattr(qmesh, "hwm_encode", lambda flat: np.full(len(flat), 70000, np.int64))
    with pytest.raises(ValueError, match="index"):
        qmesh.encode(lon, lat, h, tris, B16)
