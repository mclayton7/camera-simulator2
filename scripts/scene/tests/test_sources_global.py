import numpy as np
import rasterio
from fakes import FakeHttp, StubCache
from PIL import Image

from camsim_scene.sources import bmng, etopo2022, make_source
from camsim_scene.sources.base import Area, Asset

GLOBE = Area((-180.0, -90.0, 180.0, 90.0))


def test_etopo_has_surface_and_geoid_assets():
    http = FakeHttp({("HEAD", etopo2022.SURFACE): (200, 1585813987), ("HEAD", etopo2022.GEOID): (200, 1446300529)})
    src = make_source("etopo2022", http=http)
    assets = src.discover(GLOBE)
    assert [(a.id, a.role) for a in assets] == [
        ("ETOPO_2022_v1_30s_N90W180_surface", "data"),
        ("ETOPO_2022_v1_30s_N90W180_geoid", "geoid"),
    ]
    ras = src.open("x.tif", assets[0])
    assert ras.datum == "wgs84_egm2008_raster" and ras.clamp_edges and ras.nodata == -99999.0
    assert ras.vertical_asset == "etopo2022/ETOPO_2022_v1_30s_N90W180_geoid"
    assert src.global_coverage and src.max_zoom == 8


def test_bmng_urls_and_tile_origins():
    assert (
        bmng.url(7, "A1")
        == "https://eoimages.gsfc.nasa.gov/images/imagerecords/73000/73751/world.topo.bathy.200407.3x21600x21600.A1.png"
    )
    assert bmng.url(11, "D2").endswith("/73884/world.topo.bathy.200411.3x21600x21600.D2.png")
    assert bmng.tile_origin("A1") == (-180.0, 90.0) and bmng.tile_origin("D2") == (90.0, 0.0)
    routes = {("HEAD", bmng.url(1, t)): (200, 285_000_000) for t in bmng.TILES}
    src = make_source("bmng", options={"month": 1}, http=FakeHttp(routes))
    assets = src.discover(GLOBE)
    assert len(assets) == 8 and assets[0].metadata["bbox"] == [-180.0, 0.0, -90.0, 90.0] and src.version == "2004-01"


def test_bmng_prepare_georeferences_the_png(tmp_path):
    png = tmp_path / "b2.png"
    Image.fromarray(np.full((16, 16, 3), 9, np.uint8)).save(png)
    src = make_source("bmng", options={"month": 7}, http=FakeHttp({}))
    asset = Asset("world.topo.bathy.200407.B2", "u", sha256="12" * 32, metadata={"tile": "B2"})
    out = src.prepare(png, asset, StubCache(tmp_path))
    with rasterio.open(out) as ds:
        assert (
            ds.crs.to_epsg() == 4326
            and ds.transform.c == -90.0
            and ds.transform.f == 0.0
            and ds.transform.a == bmng.RES
        )
