import io

import numpy as np
import pytest
from fake_sources import COAST, coast_scene, fake_context, naip_ndvi_truth, naip_s2_scene, s2_ndvi_truth
from PIL import Image

from camsim_scene import tiling
from camsim_scene.context import WorkerState, class_rasters
from camsim_scene.layers import imagery, ndvi
from camsim_scene.water import Water


class Linear:
    def __init__(self, gain, offset):
        self.gain, self.offset = gain, offset

    def apply(self, n, lon, lat):
        return self.gain * np.asarray(n) + self.offset


def state(tmp_path, scene=None):
    ctx = fake_context(tmp_path, scene or naip_s2_scene(tmp_path / "src", ndvi=True))
    return WorkerState(ctx), ctx


def leaf_at(st, z, lon, lat, fit=None, water=None, interior=None):
    s = tiling.tile_size_deg(z)
    x, y = int((lon + 180) // s), int((lat + 90) // s)
    entries = st.index["ndvi"].query(imagery.query_bounds(z, x, y, 200.0))
    (glon, glat), _ = imagery.pixel_grid(z, x, y)
    return ndvi.leaf_ndvi(z, x, y, entries, fit, interior, water), glon, glat


def test_encode_decode_round_trip():
    codes = np.arange(256, dtype=np.uint8)
    assert np.array_equal(ndvi.encode(ndvi.decode(codes)), codes)
    assert ndvi.encode(np.array([-1.0, 0.0, 1.0, np.nan, 2.0, -3.0])).tolist() == [1, 128, 255, 0, 255, 1]
    assert np.isnan(ndvi.decode(np.array([0], np.uint8))[0])


def test_ndvi_of_red_and_nir():
    v = ndvi.ndvi_of(np.array([[100.0, 0.0, 50.0, np.nan], [300.0, 0.0, 50.0, 10.0]]))
    assert v[0] == pytest.approx(0.5) and np.isnan(v[1]) and v[2] == 0.0 and np.isnan(v[3])


def test_ndvi_index_reads_red_and_nir(tmp_path):
    st, _ = state(tmp_path)
    e = st.index["ndvi"].query((10.05, 10.05, 10.06, 10.06))
    assert [imagery.source_of(x) for x in e] == ["naip_pc", "wc_s2"] and all(x.raster.bands == (1, 4) for x in e)


def test_inside_naip_the_leaf_is_naip_ndvi(tmp_path):
    st, _ = state(tmp_path)
    v, lon, lat = leaf_at(st, 12, 10.1, 10.1)
    assert np.isfinite(v).all() and np.abs(v - naip_ndvi_truth(lon, lat)).max() < 0.02


def test_outside_naip_the_leaf_is_sentinel2_ndvi(tmp_path):
    st, _ = state(tmp_path)
    v, lon, lat = leaf_at(st, 12, 9.9, 9.9)
    assert np.isfinite(v).all() and np.abs(v - s2_ndvi_truth(lon, lat)).max() < 0.02


def test_the_fit_applies_to_naip_only(tmp_path):
    st, _ = state(tmp_path)
    for lon0 in (10.1, 9.9):  # inside NAIP (fitted onto Sentinel-2's scale), outside (Sentinel-2 untouched)
        v, lon, lat = leaf_at(st, 12, lon0, lon0, fit=Linear(0.9, 0.05))
        assert np.abs(v - s2_ndvi_truth(lon, lat)).max() < 0.02


def test_naip_feathers_into_sentinel2_over_200_m(tmp_path):
    st, _ = state(tmp_path)
    v, lon, lat = leaf_at(st, 14, 10.001, 10.1, fit=Linear(1.0, 0.3))  # straddles NAIP's west edge (10.0 E)
    naip, s2 = naip_ndvi_truth(lon, lat) + 0.3, s2_ndvi_truth(lon, lat)
    w = (v - s2) / (naip - s2)  # NAIP's share
    d = (lon - 10.0) * 111320.0  # metres east of the edge
    assert np.abs(w[d < -30]).max() < 0.1 and np.abs(w[d > 280] - 1).max() < 0.1
    assert 0.2 < w[(d > 80) & (d < 120)].mean() < 0.8


def test_naip_is_clipped_200_m_offshore_and_kept_on_land(tmp_path):
    st, ctx = state(tmp_path, coast_scene(tmp_path / "src", ndvi=True))
    classes, _ = class_rasters(st.manifest, ctx.asset_paths)
    water = Water(tuple(classes), 200.0, 0.0)
    v, lon, lat = leaf_at(st, 13, 10.11, 10.10, water=water)  # all sea, east of COAST
    off = (lon > COAST + 0.004) & (lon < 10.139) & (lat > 10.081) & (lat < 10.119)
    assert np.abs(v[off] - s2_ndvi_truth(lon, lat)[off]).max() < 0.02
    v, lon, lat = leaf_at(st, 13, 10.085, 10.10, water=water)  # land, with a 300 m lake (kept: within the buffer)
    land = (lon > 10.065) & (lat < 10.119)
    assert np.abs(v[land] - naip_ndvi_truth(lon, lat)[land]).max() < 0.02


def test_png_bytes_are_deterministic_grayscale():
    code = ndvi.encode(np.linspace(-1, 1, 256 * 256).reshape(256, 256))
    a = ndvi.png_bytes(code)
    assert a == ndvi.png_bytes(code.copy())
    im = Image.open(io.BytesIO(a))
    assert im.mode == "L" and im.size == (256, 256) and np.array_equal(ndvi.read_png(a), code)


def test_read_png_refuses_a_wrong_tile():
    buf = io.BytesIO()
    Image.new("RGB", (256, 256)).save(buf, "PNG")
    with pytest.raises(ValueError, match="expected L"):
        ndvi.read_png(buf.getvalue())


def test_parent_means_the_valid_pixels_of_each_block():
    cols = np.arange(256)[None, :] * np.ones((256, 1))
    a = ndvi.encode(np.full((256, 256), 0.5))
    b = ndvi.encode(np.where(cols < 128, 0.2, np.nan))
    c = ndvi.encode(np.where(np.indices((256, 256)).sum(0) % 2 == 0, 0.6, np.nan))  # checkerboard
    kids = {(0, 1): ndvi.png_bytes(a), (1, 1): ndvi.png_bytes(b), (0, 0): ndvi.png_bytes(c), (1, 0): None}
    p = ndvi.decode(ndvi.read_png(ndvi.parent_tile(kids)))
    assert np.allclose(p[:128, :128], 0.5, atol=0.005)
    assert np.allclose(p[:128, 128:192], 0.2, atol=0.005) and np.isnan(p[:128, 192:]).all()
    assert np.allclose(p[128:, :128], 0.6, atol=0.005)  # two valid pixels in every block
    assert np.isnan(p[128:, 128:]).all()


def test_tiles_without_data_produce_nothing():
    assert ndvi.parent_tile({(0, 0): None, (1, 0): None, (0, 1): None, (1, 1): None}) is None
    assert ndvi.leaf_tile(12, 0, 0, []) is None
