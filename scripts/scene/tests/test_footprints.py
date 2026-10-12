import json

import numpy as np
import shapely
from fake_sources import build_synthetic
from PIL import Image
from test_buildings_layer import ROWS, _scene

from camsim_scene import buildings as B
from camsim_scene import footprints as F

D = F.TILE_DEG / F.MASK_PX  # one mask cell, degrees
I0, J0 = 200, 200  # tile +10.00_+10.00: lat 10.00-10.05, lon 10.00-10.05


def _ring(x0, y0, x1, y1, i=I0, j=J0):
    """A rectangle in cell units of tile (i, j): x east from its west edge, y south from its north edge."""
    w, n = j * F.TILE_DEG, (i + 1) * F.TILE_DEG
    return [(w + x0 * D, n - y0 * D), (w + x1 * D, n - y0 * D), (w + x1 * D, n - y1 * D), (w + x0 * D, n - y1 * D)]


def _bld(sid, poly):
    return B.Building(sid, [poly], "", 4.0, 2, 0.0, 4.5, 100.0, (15, 0, 0))


def _centres(rows: slice, cols: slice, i=I0, j=J0):
    w, n = j * F.TILE_DEG, (i + 1) * F.TILE_DEG
    lon = w + (np.arange(F.MASK_PX)[cols] + 0.5) * D
    lat = n - (np.arange(F.MASK_PX)[rows] + 0.5) * D
    return np.meshgrid(lon, lat)


def test_polygon_with_hole_sets_exactly_the_centre_cells():
    # edges a quarter cell off the grid, so no cell centre lies on an edge
    poly = shapely.Polygon(_ring(100.25, 200.25, 160.25, 240.25), [_ring(120.25, 210.25, 130.25, 220.25)])
    m = F.rasterise_tile([poly], I0, J0)
    assert m.shape == (F.MASK_PX, F.MASK_PX) and m.dtype == bool
    assert m.sum() == 60 * 40 - 10 * 10  # cols 100..159, rows 200..239, minus the hole's cols 120..129, rows 210..219
    rows, cols = slice(190, 250), slice(90, 170)
    lon, lat = _centres(rows, cols)
    assert np.array_equal(m[rows, cols], shapely.contains_xy(poly, lon, lat))
    assert not m[215, 125] and m[205, 125]


def test_raster_skips_tiles_without_buildings_and_packs_rows():
    poly = shapely.Polygon(_ring(10.25, 10.25, 20.25, 20.25))
    r = F.raster_tiles([_bld("a", poly)])
    assert list(r) == [(I0, J0)]
    assert r[(I0, J0)].dtype == np.uint8 and r[(I0, J0)].shape == (F.MASK_PX, F.MASK_PX // 8)
    assert F.unpack(r[(I0, J0)]).sum() == 100
    assert F.raster_tiles([]) == {}


def test_mask_building_across_tile_edge_sets_both_tiles():
    # x from 10.25 cells west of the tile's east edge to 9.75 cells east of it (in the next tile)
    w, n = J0 * F.TILE_DEG, (I0 + 1) * F.TILE_DEG
    e = w + F.TILE_DEG
    poly = shapely.box(e - 10.25 * D, n - 110.25 * D, e + 9.75 * D, n - 100.25 * D)
    r = F.raster_tiles([_bld("edge", poly)])
    assert sorted(r) == [(I0, J0), (I0, J0 + 1)]
    west, east = F.unpack(r[(I0, J0)]), F.unpack(r[(I0, J0 + 1)])
    assert west.sum() == 100 and west[100:110, F.MASK_PX - 10 :].all()
    assert east.sum() == 100 and east[100:110, :10].all()


def test_burn_cells_half_cover_rule():
    m = np.zeros((F.MASK_PX, F.MASK_PX), bool)
    m[0:8, 0:8].flat[:31] = True  # land-cover cell (0, 0): 31 of 64
    m[0:8, 8:16].flat[:32] = True  # cell (0, 1): 32 of 64
    b = F.burn_cells(m)
    assert b.shape == (F.TILE_PX, F.TILE_PX)
    assert not b[0, 0] and b[0, 1] and b.sum() == 1


def test_mask_png_is_one_bit_and_round_trips(tmp_path):
    m = np.zeros((F.MASK_PX, F.MASK_PX), bool)
    m[5:9, 4790:4800] = True
    p = tmp_path / "t.png"
    F.write_mask_tile(np.packbits(m, axis=1), p)
    im = Image.open(p)
    assert im.mode == "1" and im.size == (F.MASK_PX, F.MASK_PX)
    assert np.array_equal(np.array(im), m)
    assert F.mask_png(np.packbits(m, axis=1)) == p.read_bytes()  # deterministic bytes


def test_raster_file_round_trip(tmp_path):
    r = F.raster_tiles([_bld("a", shapely.Polygon(_ring(1.25, 1.25, 3.25, 3.25)))])
    F.save_raster(r, tmp_path / "r.npz")
    back = F.load_raster(tmp_path / "r.npz")
    assert list(back) == list(r) and np.array_equal(back[(I0, J0)], r[(I0, J0)])
    F.save_raster({}, tmp_path / "e.npz")
    assert F.load_raster(tmp_path / "e.npz") == {}


def test_layer_writes_mask_tiles_and_index(tmp_path):
    pkg, _, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    d = pkg / "features/buildings/mask"
    index = json.loads((d / "index.json").read_text())
    assert index["format"] == F.MASK_FORMAT and index["tile_px"] == 4800 and index["tile_deg"] == 0.05
    assert index["cell_factor"] == 8 and index["rules"] == F.MASK_RULES_VERSION
    # b1 (60 x 50 m centred on 10.20, 10.20) straddles both tile edges; b2 straddles the 10.20 N edge
    files = ["+10.15_+10.15.png", "+10.15_+10.20.png", "+10.20_+10.15.png", "+10.20_+10.20.png"]
    assert [t["file"] for t in index["tiles"]] == files
    total = sum(int(np.array(Image.open(d / f)).sum()) for f in files)
    assert 2000 < total < 3000  # 3,184 m2 of footprint at ~1.14 m x 1.16 m cells (lat 10): ~2,400 cells
    hashes = (pkg / "hashes.txt").read_text()
    assert "features/buildings/mask/index.json" in hashes and "features/buildings/mask/+10.20_+10.20.png" in hashes


def test_plan_runs_once_per_build(tmp_path, monkeypatch):
    calls = []
    real = B.plan_buildings
    monkeypatch.setattr(B, "plan_buildings", lambda *a, **k: (calls.append(1), real(*a, **k))[1])
    build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    assert len(calls) == 1  # the GLBs and the footprint raster share one cached plan
