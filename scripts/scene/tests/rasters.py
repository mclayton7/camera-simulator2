"""Write small synthetic GeoTIFFs for tests."""

from pathlib import Path

import numpy as np
import rasterio
from rasterio.enums import Resampling
from rasterio.transform import from_origin


def write_geotiff(
    path: Path, data, west, north, res, crs="EPSG:4326", nodata=None, overviews=(2, 4), tiled=True, tags=None
) -> Path:
    data = np.asarray(data)
    if data.ndim == 2:
        data = data[None]
    profile = {
        "driver": "GTiff",
        "width": data.shape[2],
        "height": data.shape[1],
        "count": data.shape[0],
        "dtype": data.dtype,
        "crs": crs,
        "transform": from_origin(west, north, res, res),
        "nodata": nodata,
    }
    if tiled:
        profile.update(tiled=True, blockxsize=256, blockysize=256)
    with rasterio.open(path, "w", **profile) as ds:
        ds.write(data)
        if tags:
            ds.update_tags(**tags)
    if overviews:
        with rasterio.open(path, "r+") as ds:
            ds.build_overviews(list(overviews), Resampling.average)
    return Path(path)
