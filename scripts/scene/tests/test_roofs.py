import math

import numpy as np
from overture_fixture import box_lonlat

from camsim_scene import buildings as B
from camsim_scene import gltf, roofs
from camsim_scene.roofs import RoofOptions

M = 111320.0
LON0, LAT0 = 9.999, 9.999  # south-west of every test building, so encoded positions are positive
ORIGIN = (10.0, 10.0, 55.0)


class PosSampler:
    """RGB = the point's position, 1/16 m east / north of (LON0, LAT0), 12 bits each (no wrap within 256 m); RGB 0 and
    res inf where `valid` is false; native resolution `res_m` (a number, or a function of lon)."""

    def __init__(self, res_m=0.6, valid=None):
        self.res_m, self.valid = res_m, valid

    @staticmethod
    def en(lon, lat):
        return (np.asarray(lon) - LON0) * M * math.cos(math.radians(LAT0)), (np.asarray(lat) - LAT0) * M

    def sample(self, lon, lat, target_m):
        lon, lat = np.asarray(lon, float), np.asarray(lat, float)
        e, n = self.en(lon, lat)
        ex = np.round(e * 16).astype(np.int64) & 4095
        ny = np.round(n * 16).astype(np.int64) & 4095
        rgb = np.stack([ex >> 4, ((ex & 15) << 4) | (ny >> 8), ny & 255]).astype(np.uint8)
        ok = np.ones(lon.shape, bool) if self.valid is None else self.valid(lon, lat)
        rgb[:, ~ok] = 0
        res = self.res_m(lon) if callable(self.res_m) else np.full(lon.shape, float(self.res_m))
        return rgb, ok, np.where(ok, res, np.inf)

    @staticmethod
    def decode(rgb):
        rgb = rgb.astype(np.int64)
        ex = (rgb[..., 0] << 4) | (rgb[..., 1] >> 4)
        ny = ((rgb[..., 1] & 15) << 8) | rgb[..., 2]
        return ex / 16.0, ny / 16.0


def flat(h):
    return lambda lon, lat: np.full(np.shape(lon), float(h))


def _plan(fps):
    blds, _ = B.plan_buildings(fps, flat(50.0), 10.0)
    return blds


def _tex(g, sampler, opts=None):
    return roofs.roof_texture(g.positions, g.roof, g.table.source_ids, g.matrix, sampler, opts or RoofOptions())


def _two():
    ring = box_lonlat(10.0, 10.0, 30, 24).difference(box_lonlat(10.0, 10.0, 8, 6))
    return [B.Footprint("a", ring, 9.0, None, ""), B.Footprint("b", box_lonlat(10.0004, 10.0002, 12, 9), 6.0, None, "")]


def test_texcoords_sample_the_expected_source_pixels():
    g = B.tile_geometry(_plan(_two()), ORIGIN)
    s = PosSampler()
    tex = _tex(g, s)
    assert tex.textured == frozenset({0, 1}) and tex.texel_m == 0.5 and tex.n_coarse == tex.n_overflow == 0
    size = tex.rgb.shape[0]
    assert tex.rgb.shape == (size, size, 3) and size & (size - 1) == 0
    tol = tex.texel_m * (0.5 + 1e-3) + 1 / 16  # the pixel centre is within half a texel of the point; 1/16 m encoding
    for idx in g.roof.values():
        tri = idx.reshape(-1, 3)
        uv = tex.texcoords[tri].astype(np.float64).mean(axis=1)  # UV is affine in (east, north): centroid -> centroid
        p = g.positions[tri].astype(np.float64).mean(axis=1)
        lon, lat = roofs.enu_to_lonlat(g.matrix, p[:, 0], -p[:, 2], p[:, 1])
        want_e, want_n = s.en(lon, lat)
        got_e, got_n = s.decode(tex.rgb[np.floor(uv[:, 1] * size).astype(int), np.floor(uv[:, 0] * size).astype(int)])
        assert np.abs(got_e - want_e).max() <= tol and np.abs(got_n - want_n).max() <= tol
    roof_v = np.concatenate(list(g.roof.values()))
    assert (tex.texcoords[np.setdiff1d(np.arange(len(g.positions)), roof_v)] == 0).all()  # walls (0, 0)
    assert tex.texcoords[roof_v].min() >= 0 and tex.texcoords[roof_v].max() <= 1


def test_roof_texture_is_deterministic():
    g = B.tile_geometry(_plan(_two()), ORIGIN)
    a, b = _tex(g, PosSampler()), _tex(g, PosSampler())
    assert a.jpeg == b.jpeg and np.array_equal(a.texcoords, b.texcoords)


def test_coarse_source_falls_back_to_flat():
    fps = [B.Footprint("w", box_lonlat(10.0, 10.0, 20, 20), 6.0, None, ""),
           B.Footprint("e", box_lonlat(10.0006, 10.0, 20, 20), 6.0, None, "")]
    g = B.tile_geometry(_plan(fps), ORIGIN)
    tex = _tex(g, PosSampler(res_m=lambda lon: np.where(lon < 10.0003, 0.6, 10.0)))
    assert [g.table.source_ids[f] for f in sorted(tex.textured)] == ["w"] and tex.n_coarse == 1


def test_all_coarse_tile_has_no_texture_and_null_texel():
    blds = _plan(_two())
    g = B.tile_geometry(blds, ORIGIN)
    tex = _tex(g, PosSampler(res_m=10.0))
    assert tex.textured == frozenset() and tex.jpeg is None and tex.texel_m is None and tex.n_coarse == 2
    mesh, table = B.tile_mesh(blds, ORIGIN, RoofOptions(), PosSampler(res_m=10.0))
    assert mesh.texture_jpeg is None and mesh.extras == {"roof_texel_m": None} and len(mesh.textured_indices) == 0
    doc, _ = gltf.read_glb(gltf.write_glb(mesh, table, "c"))
    assert "images" not in doc and doc["extras"] == {"roof_texel_m": None} and len(doc["materials"]) == 2


def test_roof_partial_nodata_fills_with_the_roof_mean():
    g = B.tile_geometry(_plan([B.Footprint("a", box_lonlat(10.0, 10.0, 30, 20), 6.0, None, "")]), ORIGIN)
    edge = 10.0 - 5.5 / (M * math.cos(math.radians(10.0)))  # the west ~9.5 m of the 30 m roof has no imagery
    tex = _tex(g, PosSampler(valid=lambda lon, lat: lon > edge))
    assert tex.textured == frozenset({0})
    size = tex.rgb.shape[0]
    uv = tex.texcoords[g.roof[0]] * size
    x0, x1 = int(np.floor(uv[:, 0].min())), int(np.ceil(uv[:, 0].max()))
    y0, y1 = int(np.floor(uv[:, 1].min())), int(np.ceil(uv[:, 1].max()))
    block = tex.rgb[y0:y1, x0:x1].reshape(y1 - y0, x1 - x0, 3)
    assert not (block == 0).all(axis=2).any()  # never black
    west = block[:, :8].reshape(-1, 3)
    assert (west == west[0]).all()  # the invalid texels share one fill colour


def test_tile_mesh_without_roof_options_keeps_the_r3_layout():
    blds = _plan(_two())
    mesh, _ = B.tile_mesh(blds, ORIGIN)
    assert mesh.extras is None and mesh.texcoords is None and len(mesh.textured_indices) == 0
    g = B.tile_geometry(blds, ORIGIN)
    assert np.array_equal(mesh.roof_indices, np.concatenate([g.roof[f] for f in sorted(g.roof)]))


def test_tile_mesh_splits_textured_and_flat_and_counts():
    fps = [B.Footprint("w", box_lonlat(10.0, 10.0, 20, 20), 6.0, None, ""),
           B.Footprint("e", box_lonlat(10.0006, 10.0, 20, 20), 6.0, None, "")]
    blds = _plan(fps)
    stats = {}
    sampler = PosSampler(res_m=lambda lon: np.where(lon < 10.0003, 0.6, 10.0))
    mesh, table = B.tile_mesh(blds, ORIGIN, RoofOptions(), sampler, stats)
    fid = mesh.feature_ids.astype(int)
    assert set(fid[mesh.textured_indices]) == {1} and set(fid[mesh.roof_indices]) == {0}  # ids sort: e = 0, w = 1
    assert stats == {"textured": 1, "flat_coarse": 1, "flat_overflow": 0, "tiles_by_texel_m": {"0.5": 1}}
    t = gltf.decode_tile(gltf.write_glb(mesh, table, "c"))
    assert t.extras == {"roof_texel_m": 0.5} and t.texture == mesh.texture_jpeg
