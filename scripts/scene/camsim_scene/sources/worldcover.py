"""ESA WorldCover 10 m 2021 v200 land cover (3 x 3 degree COGs, CC BY 4.0) and its COG grid. The land-cover
layer cuts these COGs into CamSim's 0.05 degree tiles (layers/landcover.py). Import-light (numpy only)."""

from __future__ import annotations

from pathlib import Path

from .base import Area, Asset, Layer, SourceBase, SourceRaster

TILE_DEG = 0.05
CELLS_PER_DEG = 12000
TILE_PX = 600  # TILE_DEG * CELLS_PER_DEG
TILES_PER_COG = 60  # 3 degrees / TILE_DEG
COG_URL = (
    "https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_{name}_Map.tif"
)
SOURCE = "ESA WorldCover 10m 2021 v200"
LICENCE = "CC BY 4.0"
ATTRIBUTION = (
    "© ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) "
    "processed by ESA WorldCover consortium"
)


def cog_name(i: int, j: int) -> str:
    """The WorldCover COG holding tile (i, j), named by its 3-degree SW corner (N36W123)."""
    lat = (i // TILES_PER_COG) * 3
    lon = (j // TILES_PER_COG) * 3
    return f"{'N' if lat >= 0 else 'S'}{abs(lat):02d}{'E' if lon >= 0 else 'W'}{abs(lon):03d}"


def cog_offset(i: int, j: int) -> tuple[int, int]:
    """(row_off, col_off) in cells of tile (i, j) inside its COG (row 0 = the COG's north edge)."""
    row = (i // TILES_PER_COG) * TILES_PER_COG + TILES_PER_COG - 1 - i
    col = j - (j // TILES_PER_COG) * TILES_PER_COG
    return row * TILE_PX, col * TILE_PX


class WorldCover(SourceBase):
    id = "worldcover"
    layer = Layer.LANDCOVER
    licence = "CC-BY-4.0"
    dataset = SOURCE
    version = "2021 v200"
    attribution = ATTRIBUTION
    area_kind = "ring"
    datum = "wgs84"

    def discover(self, area: Area) -> list[Asset]:
        from ..layers.landcover import tiles_for_bbox
        from ..net import HttpError  # requests-backed: imported lazily so the module stays import-light

        names = sorted({cog_name(i, j) for i, j in tiles_for_bbox(*area.bounds)})
        assets = []
        for name in names:
            url = COG_URL.format(name=name)
            status, size = self.http.head(url)
            if status == 404:
                continue  # open ocean: the land-cover index lists its tiles as missing
            if status != 200:
                raise HttpError(f"HEAD {url}: HTTP {status}")
            assets.append(Asset(id=name, url=url, size=size, group=self.id, rank=len(assets)))
        return assets

    def prepare(self, path: Path, asset: Asset, cache) -> Path | None:
        return None  # cut exactly by window; no overviews needed

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return SourceRaster(path=Path(path), datum=self.datum)
