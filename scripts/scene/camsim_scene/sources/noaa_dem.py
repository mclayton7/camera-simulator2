"""NOAA NGDC coastal topobathy DEMs for the seabed under tidal water (REALISM R1; spec
docs/superpowers/specs/2026-10-10-r1-coast-and-gates-design.md). CUDEM has no Southern California tiles
(checked 2026-10-10), so the Pendleton package uses the San Diego 1/3" NAVD88 tsunami DEM near the coast and
the Coastal Relief Model (MSL) across the ring. Both are static netCDF files (2016 / 2018) on NCEI THREDDS;
`prepare` turns each into a COG. Public domain (US Government work); not for navigation."""

from __future__ import annotations

from pathlib import Path

from ..net import HttpError
from .base import Area, Asset, Layer, SourceBase, SourceRaster, _dataset

SD13_URL = "https://www.ngdc.noaa.gov/thredds/fileServer/regional/san_diego_13_navd88_2012.nc"
CRM_URL = "https://www.ngdc.noaa.gov/thredds/fileServer/crm/crm_socal_3as_vers2.nc"


def _intersects(a, b) -> bool:
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


class _NoaaDem(SourceBase):
    layer = Layer.TERRAIN
    licence = "LicenseRef-PublicDomain-USGov"
    area_kind = "ring"
    datum = "nad83_2011_navd88_geoid18"  # NAD83 taken as NAD83(2011) (datum.py); GEOID18 for NAVD88
    url = ""
    extent: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)
    asset_id = ""
    vertical_from_msl = False  # True: heights are MSL; plan_scene stores the station's MSL - NAVD88 per asset

    def discover(self, area: Area) -> list[Asset]:
        if not _intersects(area.bounds, self.extent):
            return []
        status, size = self.http.head(self.url)
        if status != 200:
            raise HttpError(f"HEAD {self.url}: HTTP {status}")
        if size is None:  # NCEI THREDDS omits Content-Length on HEAD; a ranged GET carries the total
            size = self.http.total_size(self.url)
        return [Asset(id=self.asset_id, url=self.url, size=size, group=self.id, metadata={"bbox": list(self.extent)})]

    def _raster(self, path: Path, add_m: float) -> SourceRaster:
        return SourceRaster(path=Path(path), datum=self.datum, nodata=_dataset(str(path), None).nodata, add_m=add_m)

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return self._raster(path, 0.0)


class NoaaSd13(_NoaaDem):
    id = "noaa_sd13"
    dataset = "NOAA NGDC San Diego, CA 1/3 arc-second NAVD 88 Coastal Digital Elevation Model"
    version = "2012"
    attribution = (
        "NOAA National Geophysical Data Center (2012): San Diego, CA 1/3 arc-second NAVD 88 Coastal Digital "
        "Elevation Model"
    )
    max_zoom = 14
    url = SD13_URL
    extent = (-117.83005, 32.44995, -116.99995, 33.60005)
    asset_id = "san_diego_13_navd88_2012"


class NoaaCrmSocal(_NoaaDem):
    id = "noaa_crm_socal"
    dataset = "NOAA NGDC U.S. Coastal Relief Model - Southern California vers. 2 (3 arc-second)"
    version = "2"
    attribution = (
        "National Geophysical Data Center, 2012. U.S. Coastal Relief Model - Southern California vers. 2. "
        "NOAA. doi:10.7289/V5V985ZM"
    )
    max_zoom = 12
    url = CRM_URL
    extent = (-128.0, 30.0, -115.0, 37.0)
    asset_id = "crm_socal_3as_vers2"
    vertical_from_msl = True

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        if "msl_above_navd88_m" not in asset.metadata:
            raise ValueError(
                f"{self.id}/{asset.id}: no msl_above_navd88_m in the asset metadata (the CRM is MSL; the package "
                "needs a [sea_level] station, re-plan)"
            )
        return self._raster(path, float(asset.metadata["msl_above_navd88_m"]))
