"""USGS 3DEP 1/3 arc-second seamless DEM (1 x 1 degree GeoTIFFs, EPSG:4269 + NAVD88, GEOID18). Uses the dated
`historical/` copies, so a manifest URL never changes content: the newest per cell, or the newest on or before
the `as_of` option (YYYYMMDD)."""

from __future__ import annotations

import logging
import re
from pathlib import Path

from .base import Area, Asset, Layer, SourceBase, SourceRaster
from .tnm import tnm_products

log = logging.getLogger(__name__)
DATASET = "National Elevation Dataset (NED) 1/3 arc-second"
URL_RE = re.compile(r"/historical/([ns]\d{2}[ew]\d{3})/USGS_13_\1_(\d{8})\.tif$")


class Dep313(SourceBase):
    id = "dep3_13"
    layer = Layer.TERRAIN
    licence = "LicenseRef-PublicDomain-USGov"
    dataset = "USGS 3DEP 1/3 arc-second DEM"
    attribution = "U.S. Geological Survey, 3D Elevation Program (3DEP)"
    max_zoom = 14
    area_kind = "ring"
    datum = "nad83_2011_navd88_geoid18"

    def discover(self, area: Area) -> list[Asset]:
        as_of = self._options.get("as_of")
        best: dict[str, tuple[str, dict]] = {}
        seen: set[str] = set()
        for it in tnm_products(self.http, area.bounds, DATASET):
            m = URL_RE.search(it.get("downloadURL", ""))
            if not m:
                continue
            cell, date = m.groups()
            seen.add(cell)
            if as_of and date > as_of:
                continue
            if cell not in best or date > best[cell][0]:
                best[cell] = (date, it)
        for cell in sorted(seen - set(best)):
            log.warning("dep3_13: cell %s has no copy on or before as_of %s; skipped", cell, as_of)
        assets = []
        for rank, cell in enumerate(sorted(best)):
            date, it = best[cell]
            bb = it["boundingBox"]
            assets.append(
                Asset(
                    id=f"USGS_13_{cell}_{date}",
                    url=it["downloadURL"],
                    size=it.get("sizeInBytes"),
                    group=self.id,
                    rank=rank,
                    metadata={"cell": cell, "date": date, "bbox": [bb["minX"], bb["minY"], bb["maxX"], bb["maxY"]]},
                )
            )
        self.version = max((a.metadata["date"] for a in assets), default="")
        return assets

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return SourceRaster(path=Path(path), datum=self.datum, nodata=-999999.0)
