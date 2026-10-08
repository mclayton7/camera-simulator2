"""Datum declarations -> pinned PROJ pipelines (target ITRF2014 geographic + ellipsoid height, EPSG:7912,
coordinate epoch 2010.0). Adapters only declare a datum id; all datum maths is here.

The pipelines are explicit strings stored in the manifest (PROJ's defaults are wrong here: EPSG:4979 picks a
GEOID03 chain 0.78 m off, EPSG:9755 silently drops the geoid; docs/realism/data-sources.md 1.4). Grids are
named by their cdn.proj.org file name, fetched into the cache, and substituted by absolute path at build
time with PROJ's network access off. Sources labelled NAD83 (EPSG:4269, 269xx) are taken as the
NAD83(2011) realisation."""

from __future__ import annotations

import re
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pyproj
from scipy.ndimage import map_coordinates

from .tiling import tile_size_deg

EPOCH = 2010.0
TARGET_CRS = "EPSG:7912"
GRID_URL = "https://cdn.proj.org/{name}"
SUBGRID = 32

# "ITRF2014 to NAD83(2011) (1)" (time-dependent Helmert, coordinate frame), as PROJ 9.8 emits it.
HELMERT_ITRF2014_TO_NAD83_2011 = (
    "+proj=helmert +x=1.0053 +y=-1.90921 +z=-0.54157 +rx=0.02678138 +ry=-0.00042027 +rz=0.01093206 "
    "+s=0.00036891 +dx=0.00079 +dy=-0.0006 +dz=-0.00144 +drx=6.667e-05 +dry=-0.00075744 +drz=-5.133e-05 "
    "+ds=-7.201e-05 +t_epoch=2010 +convention=coordinate_frame"
)
_ITRF_TO_NAD83_H = (
    "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad +step +proj=cart +ellps=GRS80 "
    f"+step {HELMERT_ITRF2014_TO_NAD83_2011} +step +inv +proj=cart +ellps=GRS80 +step +proj=unitconvert +xy_in=rad +xy_out=deg"
)


def _nad83_navd88_to_itrf(grid: str) -> str:
    return (
        "+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad "
        f"+step +proj=vgridshift +grids={grid} +multiplier=1 +step +proj=cart +ellps=GRS80 "
        f"+step +inv {HELMERT_ITRF2014_TO_NAD83_2011} +step +inv +proj=cart +ellps=GRS80 "
        "+step +proj=unitconvert +xy_in=rad +xy_out=deg"
    )


@dataclass(frozen=True)
class Datum:
    id: str
    from_itrf_h: str  # ITRF2014 lon/lat (deg) -> source geographic lon/lat (deg)
    to_itrf_3d: str | None  # source geographic lon/lat + height -> ITRF2014 lon/lat/h ("proj" vertical only)
    grids: tuple[str, ...]
    vertical: str  # "none" | "proj" | "raster_geoid"
    geographic_crs: str


DATUMS = {
    d.id: d
    for d in (
        Datum("nad83_2011", _ITRF_TO_NAD83_H, None, (), "none", "EPSG:6318"),
        Datum(
            "nad83_2011_navd88_geoid18",
            _ITRF_TO_NAD83_H,
            _nad83_navd88_to_itrf("us_noaa_g2018u0.tif"),
            ("us_noaa_g2018u0.tif",),
            "proj",
            "EPSG:6318",
        ),
        Datum(
            "nad83_2011_navd88_geoid12b",
            _ITRF_TO_NAD83_H,
            _nad83_navd88_to_itrf("us_noaa_g2012bu0.tif"),
            ("us_noaa_g2012bu0.tif",),
            "proj",
            "EPSG:6318",
        ),
        Datum("wgs84", "+proj=noop", None, (), "none", "EPSG:4326"),  # WGS 84 (G2139) == ITRF2014 at cm level
        Datum("wgs84_egm2008_raster", "+proj=noop", None, (), "raster_geoid", "EPSG:4326"),
    )
}
GEOID_DATUMS = {"GEOID18": "nad83_2011_navd88_geoid18", "GEOID12B": "nad83_2011_navd88_geoid12b"}


class DatumError(Exception):
    pass


def geoid_datum(name: str) -> str | None:
    """Datum id for a 3DEP geoid name ("GEOID18", "Geoid 12B", ...); None when unsupported (e.g. GEOID12A)."""
    return GEOID_DATUMS.get(re.sub(r"\s+", "", name or "").upper())


def manifest_section(datum_ids) -> dict:
    datums, grids = {}, {}
    for did in sorted(set(datum_ids)):
        d = DATUMS[did]
        datums[did] = {
            "from_itrf_h": d.from_itrf_h,
            "to_itrf_3d": d.to_itrf_3d,
            "grids": list(d.grids),
            "vertical": d.vertical,
            "geographic_crs": d.geographic_crs,
        }
        for g in d.grids:
            grids[g] = {"url": GRID_URL.format(name=g), "sha256": None}
    return {"target": TARGET_CRS, "epoch": EPOCH, "grids": grids, "datums": datums}


def resolve_grids(pipeline: str, grid_paths: dict[str, Path]) -> str:
    def sub(m: re.Match) -> str:
        name = m.group(1)
        if name not in grid_paths or not Path(grid_paths[name]).exists():
            raise DatumError(f"PROJ grid {name} is not in the cache (run `camsim-scene fetch`)")
        return f"+grids={Path(grid_paths[name]).resolve()}"

    return re.sub(r"\+grids=([^\s]+)", sub, pipeline)


class DatumTransform:
    def __init__(self, entry: dict, grid_paths: dict[str, Path], geoid_sampler: Callable | None = None):
        pyproj.network.set_network_enabled(False)
        self.vertical = entry["vertical"]
        self.geographic_crs = entry["geographic_crs"]
        self._noop = entry["from_itrf_h"] == "+proj=noop"
        self._h = (
            None if self._noop else pyproj.Transformer.from_pipeline(resolve_grids(entry["from_itrf_h"], grid_paths))
        )
        self._v = (
            pyproj.Transformer.from_pipeline(resolve_grids(entry["to_itrf_3d"], grid_paths))
            if entry["to_itrf_3d"]
            else None
        )
        if self.vertical == "raster_geoid" and geoid_sampler is None:
            raise DatumError("datum needs a geoid raster")
        self._geoid = geoid_sampler

    def to_source_geographic(self, lon: np.ndarray, lat: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        if self._noop:
            return np.array(lon, np.float64, copy=True), np.array(lat, np.float64, copy=True)
        lon, lat = np.asarray(lon, np.float64), np.asarray(lat, np.float64)
        x, y, _, _ = self._h.transform(lon, lat, np.zeros_like(lon), np.full_like(lon, EPOCH))
        return np.asarray(x), np.asarray(y)

    def vertical_offset(self, slon: np.ndarray, slat: np.ndarray) -> np.ndarray:
        """Ellipsoid height of the source's height zero at source-geographic lon/lat (h = H + offset)."""
        slon, slat = np.asarray(slon, np.float64), np.asarray(slat, np.float64)
        if self.vertical == "none":
            return np.zeros_like(slon)
        if self.vertical == "raster_geoid":
            return np.asarray(self._geoid(slon, slat), np.float64)
        _, _, h, _ = self._v.transform(slon, slat, np.zeros_like(slon), np.full_like(slon, EPOCH))
        return np.asarray(h)


def offset_lattice(dt: DatumTransform, z: int, lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
    """Vertical offset at ITRF lon/lat (any shape): exact on lattice nodes spaced tile_size(z)/32 and aligned to
    the global tile grid, bilinear in between. Neighbouring tiles share the nodes on their common edge."""
    lon, lat = np.asarray(lon, np.float64), np.asarray(lat, np.float64)
    if dt.vertical == "none":
        return np.zeros_like(lon)
    step = tile_size_deg(z) / SUBGRID
    i0 = int(np.floor((lon.min() + 180.0) / step))
    i1 = int(np.ceil((lon.max() + 180.0) / step))
    j0 = int(np.floor((lat.min() + 90.0) / step))
    j1 = int(np.ceil((lat.max() + 90.0) / step))
    node_lon = -180.0 + np.arange(i0, i1 + 1) * step
    node_lat = np.clip(-90.0 + np.arange(j0, j1 + 1) * step, -90.0, 90.0)
    LO, LA = np.meshgrid(node_lon, node_lat)
    nodes = dt.vertical_offset(*dt.to_source_geographic(LO, LA))
    c = (lon + 180.0) / step - i0
    r = (lat + 90.0) / step - j0
    return map_coordinates(nodes, [r, c], order=1, mode="nearest")
