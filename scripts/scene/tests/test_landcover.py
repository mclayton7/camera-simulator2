import json

import numpy as np
import rasterio
from fakes import FakeHttp
from rasterio.transform import from_origin
from rasterio.windows import Window

from camsim_scene.layers import landcover
from camsim_scene.sources import make_source
from camsim_scene.sources.base import Area
from camsim_scene.sources.worldcover import COG_URL, cog_name, cog_offset


def test_worldcover_discovers_the_cogs_of_the_ring():
    names = ["N30W120", "N33W120", "N30W117", "N33W117"]
    routes = {("HEAD", COG_URL.format(name=n)): (200, 100_000_000) for n in names if n != "N30W120"}
    assets = make_source("worldcover", http=FakeHttp(routes)).discover(Area((-118.7, 32.29, -116.16, 34.42)))
    assert sorted(a.id for a in assets) == ["N30W117", "N33W117", "N33W120"]


def test_local_reader_cuts_the_same_window_as_the_remote_reader(tmp_path):
    i, j = 755, -2450  # +37.75_-122.50, inside N36W123
    cog = tmp_path / "N36W123.tif"
    profile = {
        "driver": "GTiff",
        "width": 36000,
        "height": 36000,
        "count": 1,
        "dtype": "uint8",
        "crs": "EPSG:4326",
        "transform": from_origin(-123.0, 39.0, 1 / 12000, 1 / 12000),
        "tiled": True,
        "blockxsize": 512,
        "blockysize": 512,
        "compress": "deflate",
        "SPARSE_OK": "TRUE",
    }
    pattern = ((np.arange(600)[:, None] + 3 * np.arange(600)[None, :]) % 101).astype(np.uint8)
    row, col = cog_offset(i, j)
    with rasterio.open(cog, "w", **profile) as ds:
        ds.write(pattern, 1, window=Window(col, row, 600, 600))
    reader = landcover.local_reader({COG_URL.format(name=cog_name(i, j)): cog})
    index = landcover.fetch((-122.50, 37.75, -122.45, 37.80), tmp_path / "lc", reader=reader, cut_by="camsim-scene")
    assert index["tiles"] == [{"file": "+37.75_-122.50.png", "lat_index": 755, "lon_index": -2450}]
    assert json.loads((tmp_path / "lc" / "index.json").read_text())["format"] == "camsim-landcover-1"
    from PIL import Image

    assert np.array_equal(np.asarray(Image.open(tmp_path / "lc" / "+37.75_-122.50.png")), pattern)
    assert reader("https://missing", 0, 0) is None
