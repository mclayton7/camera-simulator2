"""WorldCover water for the imagery colour match (REALISM R1; spec section 4, "Water"): class codes read
nearest-neighbour at full resolution, so NAIP can be clipped to land plus a coastal buffer and the Sentinel-2 colour
match faded out over water. Points no raster covers, and code 0, count as land."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .sources.base import M_PER_DEG, _dataset, choose_overview, project
from .tiling import Bounds

WATER = 80  # WorldCover 2021: permanent water bodies


@dataclass(frozen=True)
class Water:
    classes: tuple  # WorldCover SourceRasters; the first one covering a point wins
    buffer_m: float  # the reference (NAIP) is used only within this distance of land; 0: everywhere
    fade_m: float  # the colour match fades out to the target's own decode over this distance of water

    @property
    def reach_m(self) -> float:
        return max(self.buffer_m, self.fade_m)


def _pixels(r, lon, lat, level: int | None = None):
    ds = _dataset(str(r.path), level)
    x, y = project(r, lon, lat)
    inv = ~ds.transform
    col = np.floor(inv.a * x + inv.b * y + inv.c).astype(np.int64)
    row = np.floor(inv.d * x + inv.e * y + inv.f).astype(np.int64)
    return ds, col, row


def _level(r, target_m: float | None) -> int | None:
    """Overview level for a read at target_m (metres per sample), or None for full resolution."""
    if target_m is None:
        return None
    ds = _dataset(str(r.path), None)
    return choose_overview(abs(ds.transform.a) * M_PER_DEG, list(ds.overviews(r.bands[0])), target_m)


def class_codes(classes, lon, lat, target_m: float | None = None) -> np.ndarray:
    """uint8 code of the pixel containing each point (no interpolation); the first raster covering a point wins; 0
    where none does. `target_m`: read the coarsest overview whose pixel is at most this size (coarse zooms); None:
    full resolution."""
    from rasterio.windows import Window

    lon, lat = np.asarray(lon, np.float64), np.asarray(lat, np.float64)
    out = np.zeros(lon.shape, np.uint8)
    done = np.zeros(lon.shape, bool)
    for r in classes:
        if done.all():
            break
        ds, col, row = _pixels(r, lon, lat, _level(r, target_m))
        hit = ~done & (col >= 0) & (col < ds.width) & (row >= 0) & (row < ds.height)
        if not hit.any():
            continue
        c0, r0 = int(col[hit].min()), int(row[hit].min())
        win = Window(c0, r0, int(col[hit].max()) - c0 + 1, int(row[hit].max()) - r0 + 1)
        data = ds.read(r.bands[0], window=win)
        out[hit] = data[row[hit] - r0, col[hit] - c0]
        done |= hit
    return out


def any_water(classes, bounds: Bounds) -> bool:
    """Whether any full-resolution pixel touching bounds (W S E N; geographic rasters) is water, in any raster: a
    cheap, conservative test before class_codes on a lattice."""
    from rasterio.windows import Window

    w, s, e, n = bounds
    for r in classes:
        ds, col, row = _pixels(r, np.array([w, e, w, e]), np.array([s, s, n, n]))
        c0, c1 = max(0, int(col.min())), min(ds.width - 1, int(col.max()))
        r0, r1 = max(0, int(row.min())), min(ds.height - 1, int(row.max()))
        if c0 > c1 or r0 > r1:
            continue
        if (ds.read(r.bands[0], window=Window(c0, r0, c1 - c0 + 1, r1 - r0 + 1)) == WATER).any():
            return True
    return False
