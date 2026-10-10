"""NDVI tile jobs (REALISM R1 chunk 2; docs/superpowers/specs/2026-10-09-ndvi-sun-metadata-design.md).

Leaves compute (NIR - red) / (NIR + red) from each source's raw values at pixel centres and merge them as the imagery
leaves do (the shared helpers in imagery.py): the reference (NAIP) first, through its fit onto the target's
(Sentinel-2's) scale, fading into the rest over `feather_m` inside its valid-data edge, and with a water mask used only
within `buffer_m` of land. No colour-match-style fade over water: where nothing is behind the reference, it is kept.
Parents are the mean of the valid child pixels in each 2 x 2 block. 8-bit grayscale PNG: 0 = nodata, 1..255 = NDVI
-1..+1. A tile with no valid pixel is not written (None)."""

from __future__ import annotations

import io

import numpy as np
from PIL import Image

from ..config import TILE_PX
from ..sources.base import M_PER_DEG
from . import imagery

NODATA = 0
SCALE = 127.0  # codes per unit of NDVI (step 1/127)


def encode(v) -> np.ndarray:
    v = np.asarray(v, np.float64)
    ok = np.isfinite(v)
    code = 1.0 + np.floor((np.clip(np.where(ok, v, 0.0), -1.0, 1.0) + 1.0) * SCALE + 0.5)
    return np.where(ok, code, NODATA).astype(np.uint8)


def decode(code) -> np.ndarray:
    c = np.asarray(code).astype(np.float64)
    return np.where(c > 0, (c - 1.0) / SCALE - 1.0, np.nan)


def ndvi_of(vals: np.ndarray) -> np.ndarray:
    """NDVI from raw (red, near-infrared) values (2, ...); NaN where either is NaN or both are 0."""
    red, nir = vals[0], vals[1]
    s = nir + red
    with np.errstate(divide="ignore", invalid="ignore"):
        return np.where(s > 0, (nir - red) / s, np.nan)


def sample_ndvi(entries, lon, lat, target_m: float):
    """NDVI merged first-valid-wins in `entries` order: (values, valid, index of the winning entry (-1 none))."""
    vals, ok, which = imagery.sample_entries(entries, lon, lat, target_m)
    v = ndvi_of(vals)
    return v, ok & np.isfinite(v), which


def water_allow(z: int, x: int, y: int, water, feather_m: float):
    """(where the reference may be used, on the feather lattice; metres to land at the pixels), or (None, None)
    without a water mask, with a zero buffer, or with no water near the tile (imagery.water_clip)."""
    dist, allow = imagery.water_clip(z, x, y, water, feather_m)
    if allow is None:
        return None, None
    return allow, imagery.pixel_distance(z, x, y, dist, feather_m)


reference_weight = imagery.reference_weight


def leaf_ndvi(z, x, y, entries, fit=None, interior=None, water=None, reference="naip_pc", feather_m=200.0):
    """NDVI per pixel (NaN = nodata)."""
    (lon, lat), d = imagery.pixel_grid(z, x, y)
    tm = d * M_PER_DEG
    ref, rest = imagery.split_reference(entries, reference)
    rn, rok, _ = sample_ndvi(ref, lon, lat, tm)
    if fit is not None:
        rn = np.where(rok, fit.apply(rn), np.nan)
    allow, dpx = water_allow(z, x, y, water, feather_m)
    if imagery.reference_only(z, x, y, rok, interior, allow):
        return rn
    wt = reference_weight(z, x, y, ref, feather_m, allow, dpx, water.buffer_m if water is not None else 0.0)
    sn, sok, _ = sample_ndvi(rest, lon, lat, tm)
    wt = imagery.merge_weight(wt, rok, sok)
    out = wt * np.nan_to_num(rn) + (1.0 - wt) * np.nan_to_num(sn)
    return np.where(rok | sok, out, np.nan)


def png_bytes(code: np.ndarray) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(np.ascontiguousarray(code, dtype=np.uint8)).save(buf, "PNG", optimize=False, compress_level=6)
    return buf.getvalue()


def read_png(data: bytes) -> np.ndarray:
    im = Image.open(io.BytesIO(data))
    im.load()
    if im.mode != "L" or im.size != (TILE_PX, TILE_PX):
        raise ValueError(f"NDVI tile is {im.mode} {im.size}, expected L {(TILE_PX, TILE_PX)}")
    return np.asarray(im, np.uint8)


def leaf_tile(z, x, y, entries, fit=None, interior=None, water=None, reference="naip_pc", feather_m=200.0):
    code = encode(leaf_ndvi(z, x, y, entries, fit, interior, water, reference, feather_m))
    return png_bytes(code) if code.any() else None


def parent_tile(children: dict) -> bytes | None:
    """Each pixel: the mean of the valid decoded values in its 2 x 2 block of the children's mosaic (child (dx, dy),
    dy = 1 north). A missing child (None) is nodata."""
    m = np.full((2 * TILE_PX, 2 * TILE_PX), np.nan)
    for (dx, dy), data in children.items():
        if data is not None:
            r0, c0 = (1 - dy) * TILE_PX, dx * TILE_PX
            m[r0 : r0 + TILE_PX, c0 : c0 + TILE_PX] = decode(read_png(data))
    q = [m[0::2, 0::2], m[1::2, 0::2], m[0::2, 1::2], m[1::2, 1::2]]
    n = sum(np.isfinite(v).astype(np.int64) for v in q)
    total = np.nan_to_num(q[0]) + np.nan_to_num(q[1]) + np.nan_to_num(q[2]) + np.nan_to_num(q[3])
    with np.errstate(divide="ignore", invalid="ignore"):
        code = encode(np.where(n > 0, total / n, np.nan))
    return png_bytes(code) if code.any() else None
