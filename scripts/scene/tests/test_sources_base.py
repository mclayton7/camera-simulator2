import numpy as np
import pytest
import rasterio
from PIL import Image
from rasters import write_geotiff

from camsim_scene import sources
from camsim_scene.sources import base
from camsim_scene.sources.base import Asset, SourceRaster


class StubCache:
    def __init__(self, root):
        self.root = root

    def derived_path(self, key, suffix):
        return self.root / "derived" / key[:2] / f"{key}{suffix}"


def plane_raster(tmp_path, west=10.0, north=11.0, res=0.01, n=100, **kw):
    r, c = np.mgrid[0:n, 0:n]
    return write_geotiff(tmp_path / "plane.tif", (3.0 * c + 7.0 * r).astype(np.float32), west, north, res, **kw)


def plane_at(lon, lat, west=10.0, north=11.0, res=0.01):
    return 3.0 * ((lon - west) / res - 0.5) + 7.0 * ((north - lat) / res - 0.5)


def test_bilinear_is_exact_on_a_plane(tmp_path):
    ras = SourceRaster(plane_raster(tmp_path), datum="wgs84")
    rng = np.random.default_rng(0)
    lon, lat = rng.uniform(10.01, 10.98, 500), rng.uniform(10.02, 10.99, 500)
    vals, ok = ras.sample(lon, lat, target_m=1.0)
    assert ok.all() and np.abs(vals[0] - plane_at(lon, lat)).max() < 1e-3


def test_points_outside_are_invalid_and_shapes_are_kept(tmp_path):
    ras = SourceRaster(plane_raster(tmp_path), datum="wgs84")
    lon, lat = np.meshgrid(np.linspace(9.0, 10.5, 7), np.linspace(10.5, 12.0, 5))
    vals, ok = ras.sample(lon, lat, target_m=1.0)
    assert vals.shape == (1, 5, 7) and ok.shape == (5, 7)
    assert not ok[lon < 10.0].any() and not ok[lat > 11.0].any() and ok[(lon > 10.1) & (lat < 10.9)].all()


def test_choose_overview_picks_the_coarsest_not_coarser_than_target():
    assert base.choose_overview(1.0, [2, 4, 8], 0.5) is None
    assert base.choose_overview(1.0, [2, 4, 8], 3.0) == 0
    assert base.choose_overview(1.0, [2, 4, 8], 100.0) == 2


def test_nodata_invalidates_its_neighbourhood_only(tmp_path):
    data = np.full((50, 50), 5.0, np.float32)
    data[20, 20] = -999999.0
    path = write_geotiff(tmp_path / "nd.tif", data, 0.0, 50.0, 1.0, nodata=-999999.0, overviews=())
    ras = SourceRaster(path, datum="wgs84", nodata=-999999.0)
    vals, ok = ras.sample(np.array([20.6, 20.2, 40.5]), np.array([29.4, 29.6, 10.5]), target_m=1e9)
    assert ok.tolist() == [False, False, True] and vals[0, 2] == 5.0


def test_clamp_edges_extends_one_pixel_only(tmp_path):
    path = write_geotiff(tmp_path / "g.tif", np.ones((180, 360), np.float32), -180.0, 90.0, 1.0, overviews=())
    lon, lat = np.array([-179.9, -180.0, 10.0]), np.array([0.0, 89.99, 0.0])
    assert SourceRaster(path, datum="wgs84").sample(lon, lat, 1e9)[1].tolist() == [False, False, True]
    assert SourceRaster(path, datum="wgs84", clamp_edges=True).sample(lon, lat, 1e9)[1].tolist() == [True, True, True]
    small = write_geotiff(tmp_path / "s.tif", np.ones((10, 10), np.float32), 0.0, 10.0, 1.0, overviews=())
    far = SourceRaster(small, datum="wgs84", clamp_edges=True).sample(np.array([12.0]), np.array([5.0]), 1e9)[1]
    assert far.tolist() == [False]


def test_all_zero_rule_for_imagery(tmp_path):
    rgb = np.full((3, 20, 20), 100, np.uint8)
    rgb[:, :, :10] = 0
    rgb[1, :, 15:] = 0  # one zero band is still data
    path = write_geotiff(tmp_path / "rgb.tif", rgb, 0.0, 20.0, 1.0, overviews=())
    ras = SourceRaster(path, datum="nad83_2011", bands=(1, 2, 3), nodata_rule="all_zero")
    _, ok = ras.sample(np.array([3.0, 12.5, 17.5]), np.array([10.0, 10.0, 10.0]), 1e9)
    assert ok.tolist() == [False, True, True]


def test_projected_raster_is_sampled_through_its_projection(tmp_path):
    import pyproj

    r, c = np.mgrid[0:200, 0:200]
    path = write_geotiff(
        tmp_path / "utm.tif",
        (c + 1000.0 * r).astype(np.float64),
        470000.0,
        3685000.0,
        1.0,
        crs="EPSG:26911",
        overviews=(),
    )
    ras = SourceRaster(path, datum="nad83_2011_navd88_geoid18")
    lon, lat = pyproj.Transformer.from_crs("EPSG:26911", "EPSG:4269", always_xy=True).transform(470100.3, 3684900.7)
    x, y = base.project(ras, np.array([lon]), np.array([lat]))
    assert x[0] == pytest.approx(470100.3, abs=1e-6) and y[0] == pytest.approx(3684900.7, abs=1e-6)
    vals, ok = ras.sample(x, y, 1.0)
    assert ok[0] and vals[0, 0] == pytest.approx((x[0] - 470000.0 - 0.5) + 1000.0 * (3685000.0 - y[0] - 0.5), abs=1e-6)


def test_prepare_tiles_strips_and_georeferences_pngs(tmp_path):
    cache = StubCache(tmp_path)
    strip = write_geotiff(
        tmp_path / "strip.tif", np.ones((1000, 2000), np.float32), 0, 10, 0.01, tiled=False, overviews=()
    )
    out = base.prepare_raster(strip, Asset("s", "file://x", sha256="ab" * 32), cache)
    with rasterio.open(out) as ds:
        assert ds.profile["tiled"] and ds.overviews(1) == [2, 4]
        assert ds.tags(ns="IMAGE_STRUCTURE").get("LAYOUT") == "COG"
    assert base.prepare_raster(strip, Asset("s", "file://x", sha256="ab" * 32), cache) == out
    cog = write_geotiff(tmp_path / "cog.tif", np.ones((300, 300), np.float32), 0, 10, 0.01)
    assert base.prepare_raster(cog, Asset("c", "file://y", sha256="cd" * 32), cache) is None
    png = tmp_path / "t.png"
    Image.fromarray(np.full((16, 16, 3), 7, np.uint8)).save(png)
    from rasterio.transform import Affine

    t = Affine(90.0 / 21600, 0, -90.0, 0, -90.0 / 21600, 0.0)
    out = base.prepare_raster(png, Asset("p", "file://z", sha256="ef" * 32), cache, crs="EPSG:4326", transform=t)
    with rasterio.open(out) as ds:
        assert ds.crs.to_epsg() == 4326 and ds.transform == t and ds.read(1)[0, 0] == 7


def test_footprint_of_half_valid_raster(tmp_path):
    data = np.full((400, 400), 5.0, np.float32)
    data[:, 200:] = -999999.0
    path = write_geotiff(tmp_path / "half.tif", data, 10.0, 14.0, 0.01, nodata=-999999.0, overviews=(2, 4))
    fp = base.footprint_lonlat(SourceRaster(path, datum="wgs84", nodata=-999999.0))
    w, s, e, _n = fp.bounds
    assert (
        w == pytest.approx(10.0, abs=0.05) and e == pytest.approx(12.0, abs=0.1) and s == pytest.approx(10.0, abs=0.05)
    )
    assert fp.area == pytest.approx(8.0, rel=0.1)


def test_footprint_of_all_nodata_is_none(tmp_path):
    path = write_geotiff(tmp_path / "empty.tif", np.zeros((3, 100, 100), np.uint8), 0, 1, 0.01, overviews=())
    assert base.footprint_lonlat(SourceRaster(path, datum="wgs84", bands=(1, 2, 3), nodata_rule="all_zero")) is None


def test_s2_decoder():
    out = base.DECODERS["s2_reflectance"](np.array([[0.0, 3000.0, 6000.0, np.nan]]))
    assert out.dtype == np.uint8 and out.tolist() == [[0, 255, 255, 0]]


def test_registry_builds_by_module_path_and_rejects_unknown():
    src = sources.make_source("my_id", "camsim_scene.sources.base:SourceBase", {"k": 1})
    assert src.id == "my_id" and src.options() == {"k": 1}
    with pytest.raises(sources.UnknownSource):
        sources.make_source("nope")


def test_prepare_requires_sha256_and_ignores_stale_part_files(tmp_path):
    cache = StubCache(tmp_path)
    strip = write_geotiff(
        tmp_path / "strip.tif", np.ones((600, 600), np.float32), 0, 10, 0.01, tiled=False, overviews=()
    )
    with pytest.raises(ValueError):
        base.prepare_raster(strip, Asset("s", "file://x"), cache)
    asset = Asset("s", "file://x", sha256="ab" * 32)
    out = base.prepare_raster(strip, asset, cache)
    out.unlink()
    stale = out.with_name(out.name + ".part")
    stale.write_bytes(b"truncated")
    out2 = base.prepare_raster(strip, asset, cache)
    with rasterio.open(out2) as ds:
        assert ds.width == 600
    assert stale.read_bytes() == b"truncated"
