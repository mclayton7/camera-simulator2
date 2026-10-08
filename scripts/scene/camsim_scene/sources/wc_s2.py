"""ESA WorldCover 2021 Sentinel-2 RGBNIR median composite (10 m, 1 x 1 degree COGs, CC BY 4.0): the ring
imagery (z9-10) and the fill where NAIP stops. No tiles over open ocean (404). Each file states its licence
in a GeoTIFF tag; prepare() refuses one that doesn't say CC BY 4.0."""

from __future__ import annotations

import math
from pathlib import Path

from ..licences import LicenceError
from ..net import HttpError
from .base import Area, Asset, Layer, SourceBase, SourceRaster

URL = "https://esa-worldcover-s2.s3.eu-central-1.amazonaws.com/rgbnir/2021/{lat}/ESA_WorldCover_10m_2021_v200_{name}_S2RGBNIR.tif"
ATTRIBUTION = (
    "© ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) "
    "processed by ESA WorldCover consortium"
)


def cell_name(lat_i: int, lon_i: int) -> str:
    return f"{'N' if lat_i >= 0 else 'S'}{abs(lat_i):02d}{'E' if lon_i >= 0 else 'W'}{abs(lon_i):03d}"


class WcS2(SourceBase):
    id = "wc_s2"
    layer = Layer.IMAGERY
    licence = "CC-BY-4.0"
    dataset = "ESA WorldCover 2021 Sentinel-2 RGBNIR composite"
    version = "2021 v200"
    attribution = ATTRIBUTION
    max_zoom = 13
    area_kind = "ring"
    datum = "wgs84"

    def discover(self, area: Area) -> list[Asset]:
        w, s, e, n = area.bounds
        assets = []
        for lat_i in range(math.floor(s), math.ceil(n)):
            for lon_i in range(math.floor(w), math.ceil(e)):
                name = cell_name(lat_i, lon_i)
                url = URL.format(lat=name[:3], name=name)
                status, size = self.http.head(url)
                if status == 404:
                    continue
                if status != 200:
                    raise HttpError(f"HEAD {url}: HTTP {status}")
                assets.append(
                    Asset(
                        id=name,
                        url=url,
                        size=size,
                        group=self.id,
                        rank=len(assets),
                        metadata={"bbox": [lon_i, lat_i, lon_i + 1, lat_i + 1]},
                    )
                )
        return assets

    def prepare(self, path: Path, asset: Asset, cache) -> Path | None:
        import rasterio

        with rasterio.open(path) as ds:
            lic = ds.tags().get("license", "")
        if not lic.startswith("CC-BY 4.0"):
            raise LicenceError(f"wc_s2 {asset.id}: licence tag {lic!r}, expected CC-BY 4.0")
        return super().prepare(path, asset, cache)

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return SourceRaster(
            path=Path(path),
            datum=self.datum,
            bands=(1, 2, 3),
            nodata=0,
            nodata_rule="all_zero",
            decode="s2_reflectance",
        )
