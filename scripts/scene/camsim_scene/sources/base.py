"""Source adapter interface, and the raster sampling every layer uses.

Adapters declare a datum id (datum.py does the maths), open cached files as SourceRasters, and may sign URLs
(Planetary Computer) or prepare derived files (overviews, georeferencing). Keep module-level imports to
numpy: the land-cover wrapper imports sources.worldcover in a minimal environment."""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from enum import StrEnum
from functools import lru_cache
from pathlib import Path
from typing import Protocol

import numpy as np

from ..fsutil import sha256_bytes

M_PER_DEG = 111320.0
PREPARE_VERSION = "1"
Bounds = tuple[float, float, float, float]


class Layer(StrEnum):
    TERRAIN = "terrain"
    IMAGERY = "imagery"
    LANDCOVER = "landcover"


@dataclass(frozen=True)
class Area:
    bounds: Bounds


@dataclass
class Asset:
    id: str
    url: str
    size: int | None = None
    group: str = ""
    rank: int = 0
    role: str = "data"
    metadata: dict = field(default_factory=dict)
    sha256: str | None = None


class Source(Protocol):
    id: str
    layer: Layer
    licence: str
    dataset: str
    version: str
    attribution: str
    max_zoom: int
    global_coverage: bool
    area_kind: str  # "globe" | "ring" | "bbox": where discovery looks
    datum: str | None

    def options(self) -> dict: ...
    def discover(self, area: Area) -> list[Asset]: ...
    def open(self, path: Path, asset: Asset) -> SourceRaster: ...
    def sign(self, url: str, force: bool = False) -> str: ...
    def prepare(self, path: Path, asset: Asset, cache) -> Path | None: ...


class SourceBase:
    id = ""
    layer: Layer = Layer.TERRAIN
    licence = "LicenseRef-PublicDomain-USGov"
    dataset = ""
    version = ""
    attribution = ""
    max_zoom = 0
    global_coverage = False
    area_kind = "bbox"
    datum: str | None = None

    def __init__(self, options: dict | None = None, http=None):
        self._options = dict(options or {})
        self.http = http

    def options(self) -> dict:
        return dict(self._options)

    def discover(self, area: Area) -> list[Asset]:
        return []

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        raise NotImplementedError

    def sign(self, url: str, force: bool = False) -> str:
        return url

    def prepare(self, path: Path, asset: Asset, cache) -> Path | None:
        return prepare_raster(path, asset, cache)


S2_WHITE_DN = 3000.0  # reflectance 0.30 (DN x 1e-4) -> white
S2_GAMMA = 2.2


def _uint8(v: np.ndarray) -> np.ndarray:
    return np.clip(np.rint(np.nan_to_num(v)), 0, 255).astype(np.uint8)


def _s2_reflectance(v: np.ndarray) -> np.ndarray:
    r = np.clip(np.nan_to_num(v) / S2_WHITE_DN, 0.0, 1.0) ** (1.0 / S2_GAMMA)
    return np.rint(r * 255.0).astype(np.uint8)


DECODERS = {None: _uint8, "s2_reflectance": _s2_reflectance}


@lru_cache(maxsize=64)
def _dataset(path: str, level: int | None):
    import rasterio

    return rasterio.open(path) if level is None else rasterio.open(path, overview_level=level)


@lru_cache(maxsize=64)
def _projector(crs_text: str):
    import pyproj

    crs = pyproj.CRS.from_user_input(crs_text)
    if crs.is_geographic:
        return None
    return pyproj.Transformer.from_crs(crs.geodetic_crs, crs, always_xy=True)  # projection only, no datum change


def _span(v: np.ndarray, n: int) -> tuple[int, int] | None:
    """Pixel index range [lo, hi] (at least two pixels) covering v's bilinear neighbourhoods, or None if outside."""
    lo = max(0, int(np.floor(v.min())))
    hi = min(n - 1, int(np.floor(v.max())) + 1)
    if lo > hi or n < 2:
        return None
    if hi == lo:
        lo = max(0, hi - 1)
        hi = lo + 1
    return lo, hi


def choose_overview(res_m: float, factors: list[int], target_m: float) -> int | None:
    level = None
    for i, f in enumerate(factors):
        if res_m * f <= target_m:
            level = i
    return level


@dataclass
class SourceRaster:
    path: Path
    datum: str
    bands: tuple[int, ...] = (1,)
    nodata: float | None = None
    nodata_rule: str = "value"  # "value": band 1 is nodata or non-finite; "all_zero": every band is 0
    crs: str | None = None  # None: the file's own CRS
    clamp_edges: bool = False  # extend edge pixels by up to one pixel (global rasters, tile seams)
    decode: str | None = None  # DECODERS key (imagery)
    vertical_asset: str | None = None  # "<source>/<asset>" of a geoid raster (raster_geoid datum)

    def file_crs(self) -> str:
        return self.crs or _dataset(str(self.path), None).crs.to_wkt()

    def valid_mask(self, data: np.ndarray) -> np.ndarray:
        if self.nodata_rule == "all_zero":
            return np.any(data != 0, axis=0)
        v = data[0]
        ok = np.isfinite(v)
        if self.nodata is not None:
            ok &= v != self.nodata
        return ok

    def sample(self, x, y, target_m: float) -> tuple[np.ndarray, np.ndarray]:
        """Bilinear values at source-CRS coordinates (any shape). A point is valid when its four neighbours are."""
        from rasterio.windows import Window

        x, y = np.asarray(x, np.float64), np.asarray(y, np.float64)
        shape, nb = x.shape, len(self.bands)
        out = np.full((nb, *shape), np.nan)
        valid = np.zeros(shape, bool)
        ds0 = _dataset(str(self.path), None)
        res = abs(ds0.transform.a)
        res_m = res * M_PER_DEG if ds0.crs is None or ds0.crs.is_geographic else res
        # Approximation of the spec's area-average down: the coarsest average overview not coarser than target, then bilinear.
        level = choose_overview(res_m, ds0.overviews(self.bands[0]), target_m)
        ds = ds0 if level is None else _dataset(str(self.path), level)
        inv = ~ds.transform
        col = inv.a * x + inv.b * y + inv.c - 0.5
        row = inv.d * x + inv.e * y + inv.f - 0.5
        if self.clamp_edges:
            col = np.where((col >= -1.0) & (col <= ds.width), np.clip(col, 0.0, ds.width - 1.0), col)
            row = np.where((row >= -1.0) & (row <= ds.height), np.clip(row, 0.0, ds.height - 1.0), row)
        fin = np.isfinite(col) & np.isfinite(row)
        if not fin.any():
            return out, valid
        cs, rs = _span(col[fin], ds.width), _span(row[fin], ds.height)
        if cs is None or rs is None:
            return out, valid
        (c0, c1), (r0, r1) = cs, rs
        data = ds.read(list(self.bands), window=Window(c0, r0, c1 - c0 + 1, r1 - r0 + 1)).astype(np.float64)
        ok_px = self.valid_mask(data)
        h, w = ok_px.shape
        lc, lr = np.where(fin, col - c0, -10.0), np.where(fin, row - r0, -10.0)
        j0 = np.minimum(np.floor(lc).astype(np.int64), w - 2)
        i0 = np.minimum(np.floor(lr).astype(np.int64), h - 2)
        fc, fr = lc - j0, lr - i0
        inside = fin & (j0 >= 0) & (i0 >= 0) & (fc <= 1.0) & (fr <= 1.0)
        j0c, i0c = np.clip(j0, 0, w - 2), np.clip(i0, 0, h - 2)
        corners = [(i0c, j0c), (i0c, j0c + 1), (i0c + 1, j0c), (i0c + 1, j0c + 1)]
        ok = inside.copy()
        for ii, jj in corners:
            ok &= ok_px[ii, jj]
        wts = [(1 - fr) * (1 - fc), (1 - fr) * fc, fr * (1 - fc), fr * fc]
        for b in range(nb):
            v = sum(wt * data[b][ii, jj] for wt, (ii, jj) in zip(wts, corners))
            out[b] = np.where(ok, v, np.nan)
        return out, ok


def project(raster: SourceRaster, slon, slat) -> tuple[np.ndarray, np.ndarray]:
    """Source-geographic lon/lat -> the raster's CRS (identity for geographic rasters)."""
    t = _projector(raster.file_crs())
    if t is None:
        return np.asarray(slon, np.float64), np.asarray(slat, np.float64)
    x, y = t.transform(slon, slat)
    return np.asarray(x), np.asarray(y)


def to_uint8(raster: SourceRaster, values: np.ndarray) -> np.ndarray:
    return DECODERS[raster.decode](values)


def prepare_raster(
    path: Path, asset: Asset, cache, crs=None, transform=None, resampling: str = "average"
) -> Path | None:
    """A Cloud Optimized GeoTIFF (OGC 21-026; deflate, 512 px blocks, average overviews) derived from `path`, or None
    when the file is already tiled with overviews. `crs`/`transform` georeference files that carry none (Blue Marble)."""
    import warnings

    import rasterio
    import rasterio.shutil
    from rasterio.errors import NotGeoreferencedWarning
    from rasterio.windows import Window

    with warnings.catch_warnings():
        if crs is not None or transform is not None:
            warnings.simplefilter("ignore", NotGeoreferencedWarning)  # the caller georeferences it below
        src = rasterio.open(path)
    with src as ds:
        ready = ds.profile.get("tiled", False) and (ds.overviews(1) or max(ds.width, ds.height) <= 1024)
        if crs is None and transform is None and ready:
            return None
        key = sha256_bytes(
            f"{asset.sha256}|{PREPARE_VERSION}|{crs}|{tuple(transform) if transform else None}|{resampling}".encode()
        )
        dst = cache.derived_path(key, ".tif")
        if dst.exists():
            return dst
        dst.parent.mkdir(parents=True, exist_ok=True)
        integer = np.issubdtype(np.dtype(ds.dtypes[0]), np.integer)
        # GDAL's COG driver only copies, so stream the source (row strips: PNGs decode sequentially) into a tiled
        # intermediate first, then let the driver add overviews and lay the file out.
        profile = {
            "driver": "GTiff",
            "width": ds.width,
            "height": ds.height,
            "count": ds.count,
            "dtype": ds.dtypes[0],
            "crs": crs or ds.crs,
            "transform": transform or ds.transform,
            "nodata": ds.nodata,
            "tiled": True,
            "blockxsize": 512,
            "blockysize": 512,
            "compress": "deflate",
            "BIGTIFF": "IF_SAFER",
        }
        tmp = dst.with_name(dst.name + ".stage.tif")
        with rasterio.open(tmp, "w", **profile) as out:
            for row in range(0, ds.height, 512):
                win = Window(0, row, ds.width, min(512, ds.height - row))
                out.write(ds.read(window=win), window=win)
    part = dst.with_name(dst.name + ".part")
    try:
        rasterio.shutil.copy(
            tmp,
            part,
            driver="COG",
            COMPRESS="DEFLATE",
            PREDICTOR="2" if integer else "3",
            BLOCKSIZE="512",
            OVERVIEW_RESAMPLING=resampling.upper(),
            BIGTIFF="IF_SAFER",
            NUM_THREADS="1",
        )
        os.replace(part, dst)
    finally:
        tmp.unlink(missing_ok=True)
        part.unlink(missing_ok=True)
    return dst


def footprint_lonlat(raster: SourceRaster):
    """Valid-data polygon in lon/lat from the coarsest overview with >= 64 px, dilated by one pixel,
    simplified to half a pixel and snapped to 1e-7 degrees. None when the raster holds no data."""
    import pyproj
    import rasterio.features
    import shapely
    import shapely.geometry
    from scipy.ndimage import binary_dilation

    ds0 = _dataset(str(raster.path), None)
    level = None
    for i, f in enumerate(ds0.overviews(raster.bands[0])):
        if min(ds0.width, ds0.height) / f >= 64:
            level = i
    ds = ds0 if level is None else _dataset(str(raster.path), level)
    valid = raster.valid_mask(ds.read(list(raster.bands)).astype(np.float64))
    if not valid.any():
        return None
    valid = binary_dilation(valid, iterations=1)
    shapes = rasterio.features.shapes(valid.astype(np.uint8), mask=valid, transform=ds.transform)
    geom = shapely.union_all([shapely.geometry.shape(g) for g, v in shapes if v == 1])
    crs = pyproj.CRS.from_user_input(raster.file_crs())
    res = abs(ds.transform.a)
    if crs.is_geographic:
        tol = res / 2
    else:
        geom = shapely.segmentize(geom, res * 8)
        t = pyproj.Transformer.from_crs(crs, "EPSG:4326", always_xy=True)
        geom = shapely.transform(geom, lambda xy: np.column_stack(t.transform(xy[:, 0], xy[:, 1])))
        tol = res / M_PER_DEG / 2
    geom = shapely.set_precision(geom.simplify(tol), 1e-7)
    return None if geom.is_empty else geom
