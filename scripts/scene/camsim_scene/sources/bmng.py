"""NASA Blue Marble Next Generation, topography + bathymetry, 500 m (2004 monthly composites, eight 90 x 90
degree PNG tiles of 21600 x 21600). Global base imagery to z8. The PNGs carry no georeferencing; prepare()
writes tiled GeoTIFFs with overviews (EPSG:4326, 1/240 degree pixels)."""

from __future__ import annotations

from pathlib import Path

from ..net import HttpError
from .base import Area, Asset, Layer, SourceBase, SourceRaster, prepare_raster

MONTH_RECORDS = {
    1: 73580,
    2: 73605,
    3: 73630,
    4: 73655,
    5: 73701,
    6: 73726,
    7: 73751,
    8: 73776,
    9: 73801,
    10: 73826,
    11: 73884,
    12: 73909,
}
TILES = ("A1", "A2", "B1", "B2", "C1", "C2", "D1", "D2")
RES = 90.0 / 21600


def url(month: int, tile: str) -> str:
    rec = MONTH_RECORDS[month]
    return (
        f"https://eoimages.gsfc.nasa.gov/images/imagerecords/{rec // 1000 * 1000}/{rec}/"
        f"world.topo.bathy.2004{month:02d}.3x21600x21600.{tile}.png"
    )


def tile_origin(tile: str) -> tuple[float, float]:
    """(west, north) of a tile: columns A-D from 180 W in 90 degree steps; row 1 north of the equator."""
    return -180.0 + 90.0 * "ABCD".index(tile[0]), 90.0 - 90.0 * (int(tile[1]) - 1)


class Bmng(SourceBase):
    id = "bmng"
    layer = Layer.IMAGERY
    licence = "LicenseRef-PublicDomain-USGov"
    dataset = "NASA Blue Marble Next Generation (topography and bathymetry), 500 m"
    attribution = "NASA Earth Observatory / Reto Stöckli, Blue Marble Next Generation"
    max_zoom = 8
    global_coverage = True
    area_kind = "globe"
    datum = "wgs84"

    def discover(self, area: Area) -> list[Asset]:
        month = int(self._options.get("month", 7))
        self.version = f"2004-{month:02d}"
        out = []
        for rank, tile in enumerate(TILES):
            u = url(month, tile)
            status, size = self.http.head(u)
            if status != 200:
                raise HttpError(f"HEAD {u}: HTTP {status}")
            w, n = tile_origin(tile)
            out.append(
                Asset(
                    id=f"world.topo.bathy.2004{month:02d}.{tile}",
                    url=u,
                    size=size,
                    group=self.id,
                    rank=rank,
                    metadata={"tile": tile, "month": month, "bbox": [w, n - 90.0, w + 90.0, n]},
                )
            )
        return out

    def prepare(self, path: Path, asset: Asset, cache) -> Path | None:
        from rasterio.transform import Affine

        w, n = tile_origin(asset.metadata["tile"])
        return prepare_raster(path, asset, cache, crs="EPSG:4326", transform=Affine(RES, 0.0, w, 0.0, -RES, n))

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return SourceRaster(path=Path(path), datum=self.datum, bands=(1, 2, 3), clamp_edges=True)
