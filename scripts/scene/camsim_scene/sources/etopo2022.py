"""NOAA ETOPO 2022 30 arc-second surface elevation (land + bathymetry, EGM2008 heights) with ETOPO's own
geoid-height grid for the conversion to ellipsoid heights. Global base terrain to z8 and the fallback
everywhere. Public domain (US Government work); NCEI asks for the DOI citation."""

from __future__ import annotations

from pathlib import Path

from ..net import HttpError
from .base import Area, Asset, Layer, SourceBase, SourceRaster

BASE = "https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/30s"
SURFACE = f"{BASE}/30s_surface_elev_gtif/ETOPO_2022_v1_30s_N90W180_surface.tif"
GEOID = f"{BASE}/30s_geoid_gtif/ETOPO_2022_v1_30s_N90W180_geoid.tif"
GEOID_ID = "ETOPO_2022_v1_30s_N90W180_geoid"


class Etopo2022(SourceBase):
    id = "etopo2022"
    layer = Layer.TERRAIN
    licence = "LicenseRef-PublicDomain-USGov"
    dataset = "NOAA ETOPO 2022 30 arc-second surface elevation"
    version = "v1"
    attribution = (
        "NOAA National Centers for Environmental Information (2022): ETOPO 2022 Global Relief Model, "
        "doi:10.25921/fd45-gt74"
    )
    max_zoom = 8
    global_coverage = True
    area_kind = "globe"
    datum = "wgs84_egm2008_raster"

    def discover(self, area: Area) -> list[Asset]:
        out = []
        for rank, (aid, url, role) in enumerate(
            [("ETOPO_2022_v1_30s_N90W180_surface", SURFACE, "data"), (GEOID_ID, GEOID, "geoid")]
        ):
            status, size = self.http.head(url)
            if status != 200:
                raise HttpError(f"HEAD {url}: HTTP {status}")
            out.append(
                Asset(
                    id=aid,
                    url=url,
                    size=size,
                    group=self.id,
                    rank=rank,
                    role=role,
                    metadata={"bbox": [-180.0, -90.0, 180.0, 90.0]},
                )
            )
        return out

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return SourceRaster(
            path=Path(path), datum=self.datum, nodata=-99999.0, clamp_edges=True, vertical_asset=f"{self.id}/{GEOID_ID}"
        )
