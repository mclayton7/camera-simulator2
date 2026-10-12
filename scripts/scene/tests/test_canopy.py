"""CHMv2 canopy layer (REALISM R4): quadkeys, per-texel cover/p90 height, the layer build and verify."""

import json

import numpy as np
import pytest
import rasterio
from fake_sources import ADAPTER, build_synthetic, synthetic_scene
from fakes import FakeHttp
from PIL import Image
from rasterio.transform import from_origin

from camsim_scene.config import ConfigError, parse_scene
from camsim_scene.layers import canopy
from camsim_scene.sources import chmv2, make_source
from camsim_scene.sources.base import Area
from camsim_scene.verify import verify

R = 6378137.0
BASE = {"name": "t", "bbox": [-117.4, 33.2, -117.3, 33.3]}


def merc(lon, lat):
    return np.radians(lon) * R, R * np.log(np.tan(np.pi / 4 + np.radians(lat) / 2))


def write_chm(path, w, s, e, n, fn, res=1.2):
    """A uint8 EPSG:3857 CHM over the lon/lat box, height fn(lon) per column."""
    x0, y1 = merc(w, n)
    x1, y0 = merc(e, s)
    nx, ny = int((x1 - x0) / res) + 1, int((y1 - y0) / res) + 1
    lon = np.degrees((x0 + (np.arange(nx) + 0.5) * res) / R)
    data = np.repeat(fn(lon).astype(np.uint8)[None, :], ny, axis=0)
    with rasterio.open(path, "w", driver="GTiff", width=nx, height=ny, count=1, dtype="uint8", crs="EPSG:3857",
                       transform=from_origin(x0, y1, res, res), tiled=True, blockxsize=512, blockysize=512,
                       compress="deflate") as ds:
        ds.write(data, 1)
    return path


def test_quadkey_of_pendleton():
    assert chmv2.quadkeys_for_bbox(-117.36, 33.29, -117.34, 33.31) == ["0230132032"]


def test_quadkeys_cover_a_bbox_crossing_tiles():
    qks = chmv2.quadkeys_for_bbox(-117.62, 33.19, -117.24, 33.52)
    assert 2 <= len(qks) <= 6 and qks == sorted(qks)
    assert "0230132032" in qks


def test_quadkey_bounds_contain_the_point():
    x, y = merc(-117.35, 33.30)
    minx, miny, maxx, maxy = chmv2.quadkey_bounds_3857("0230132032")
    assert minx <= x <= maxx and miny <= y <= maxy
    assert maxx - minx == pytest.approx(2 * np.pi * R / 1024)


def test_discovery_skips_missing_quadkeys_and_reads_dates():
    qks = chmv2.quadkeys_for_bbox(-117.62, 33.19, -117.24, 33.52)
    routes = {("HEAD", chmv2.CHM_URL.format(qk=q)): (200, 141_000_000) for q in qks[1:]}
    for q in qks[1:]:
        routes[chmv2.META_URL.format(qk=q)] = {"features": [{"properties": {"acq_date": "2020-01-05"}},
                                                           {"properties": {"acq_date": "2019-06-30"}}]}
    assets = make_source("chmv2", http=FakeHttp(routes)).discover(Area((-117.62, 33.19, -117.24, 33.52)))
    assert [a.id for a in assets] == qks[1:]
    assert assets[0].metadata["acquired"] == ["2019-06-30", "2020-01-05"]
    assert assets[0].size == 141_000_000


def test_texel_stats_cover_and_p90():
    i, j = 666, -2348                       # tile +33.30_-117.40
    west, north = j * 0.05, (i + 1) * 0.05
    # 10 pixel centres inside texel (row 0, col 0): heights 0..9 -> cover 7/10 (>= 3), p90 of [3..9] = sorted[floor(0.9*6)] = 8
    lon = west + (np.arange(10) % 5 + 0.5) / 12000 / 5
    lat = north - (np.arange(10) // 5 + 0.5) / 12000 / 2
    cover, height = canopy.texel_stats(lon, lat, np.arange(10, dtype=np.uint8), i, j)
    assert cover.shape == (600, 600) and cover.dtype == np.uint8
    assert cover[0, 0] == 70 and height[0, 0] == 8
    assert cover[0, 1] == 0 and height[0, 1] == 0           # no pixels


def test_texel_stats_ignores_pixels_outside_the_tile():
    i, j = 666, -2348
    lon = np.array([j * 0.05 - 1e-6])
    lat = np.array([(i + 0.5) * 0.05])
    cover, _ = canopy.texel_stats(lon, lat, np.array([20], np.uint8), i, j)
    assert cover.sum() == 0


def test_tile_from_synthetic_chm(tmp_path):
    i, j = 666, -2348
    mid = (j + 0.5) * 0.05
    p = write_chm(tmp_path / "0230132032.tif", j * 0.05 - 0.001, i * 0.05 - 0.001, (j + 1) * 0.05 + 0.001,
                  (i + 1) * 0.05 + 0.001, lambda lon: np.where(lon < mid, 10, 0))
    cover, height = canopy.tile_from_chm(i, j, [p])
    assert cover[:, :290].min() == 100 and height[:, :290].min() == 10
    assert cover[:, 310:].max() == 0
    again = canopy.tile_from_chm(i, j, [p])
    assert np.array_equal(again[0], cover) and np.array_equal(again[1], height)


def test_texel_with_no_chm_is_zero(tmp_path):
    cover, height = canopy.tile_from_chm(666, -2348, [])
    assert cover.max() == 0 and height.max() == 0
    far = write_chm(tmp_path / "far.tif", 10.0, 10.0, 10.01, 10.01, lambda lon: np.full(lon.shape, 9))
    cover, _ = canopy.tile_from_chm(666, -2348, [far])     # a CHM file elsewhere is skipped
    assert cover.max() == 0


def test_canopy_table():
    assert parse_scene({**BASE, "canopy": {"enabled": True}}).priorities["canopy"] == ["chmv2"]
    assert parse_scene(BASE).priorities["canopy"] == []
    assert parse_scene({**BASE, "canopy": {"enabled": False}}).priorities["canopy"] == []
    with pytest.raises(ConfigError):
        parse_scene({**BASE, "canopy": {"enabled": True, "bbox": [0, 0, 1, 1]}})
    with pytest.raises(ConfigError):
        parse_scene({**BASE, "canopy": {"enabled": "yes"}})
    with pytest.raises(ConfigError):
        parse_scene({**BASE, "priorities": {"canopy": ["chmv2"]}})


def _canopy_scene(tmp_path):
    scene = synthetic_scene(tmp_path / "src", bbox=(10.0, 10.0, 10.1, 10.05))
    chm = write_chm(tmp_path / "src" / "chm.tif", 10.0, 10.0, 10.03, 10.03, lambda lon: np.where(lon < 10.015, 12, 0))
    scene["canopy"] = {"enabled": True}
    scene["sources"]["chmv2"] = {"adapter": ADAPTER, "layer": "canopy", "max_zoom": 0,
                                 "files": [{"id": "chm", "path": str(chm), "bbox": [10.0, 10.0, 10.03, 10.03]}]}
    return scene


def test_build_writes_both_tile_sets_and_skips_on_rebuild(tmp_path):
    pkg, cache, info = build_synthetic(tmp_path, _canopy_scene(tmp_path))
    for band in ("cover", "height"):
        idx = json.loads((pkg / "canopy" / band / "index.json").read_text())
        assert idx["format"] == "camsim-canopy-1" and idx["band"] == band and idx["tile_deg"] == 0.05
        assert len(idx["tiles"]) == 2                                    # 10.0-10.1 x 10.0-10.05
        assert all((pkg / "canopy" / band / t["file"]).is_file() for t in idx["tiles"])
    cov = np.array(Image.open(pkg / "canopy" / "cover" / "+10.00_+10.00.png"))
    hgt = np.array(Image.open(pkg / "canopy" / "height" / "+10.00_+10.00.png"))
    assert cov.shape == (600, 600) and cov[-100:, :150].min() == 100 and hgt[-100:, :150].min() == 12
    assert cov[:, 400:].max() == 0
    assert "synthetic test data" in (pkg / "canopy" / "ATTRIBUTION.txt").read_text()
    assert "canopy/cover/index.json" in (pkg / "hashes.txt").read_text()
    assert info["layers"]["canopy"]["built"] == 2
    report = verify(pkg)
    assert report["ok"], report
    assert any(c["name"] == "canopy" for c in report["checks"])
    from camsim_scene.pipeline import build_scene
    again = build_scene(pkg, cache, jobs=1)
    assert again["layers"]["canopy"]["skipped"] == 2 and again["layers"]["canopy"]["built"] == 0


def test_verify_catches_a_bad_canopy_tile(tmp_path):
    pkg, _cache, _ = build_synthetic(tmp_path, _canopy_scene(tmp_path))
    Image.fromarray(np.full((600, 600), 101, np.uint8)).save(pkg / "canopy" / "cover" / "+10.00_+10.00.png")
    report = verify(pkg)
    assert not next(c for c in report["checks"] if c["name"] == "canopy")["ok"]


def test_no_canopy_table_no_layer(tmp_path):
    pkg, _cache, info = build_synthetic(tmp_path)
    assert not (pkg / "canopy").exists() and "canopy" not in info["layers"]
