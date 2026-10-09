"""Imagery tile jobs. Leaves (tiles without children) sample their sources at pixel centres in priority order,
nodata falling through; parents are a 2 x 2 box filter of their four children (always four: complete siblings).
JPEG q85 4:2:0 via Pillow. With a colour match (balance.py), the target source is decoded through it and the
reference (NAIP) fades into the next source over `feather_m` inside its valid-data edge."""

from __future__ import annotations

import io
import math

import numpy as np
import shapely
from PIL import Image
from scipy.ndimage import distance_transform_edt, map_coordinates

from ..config import TILE_PX
from ..sources.base import M_PER_DEG, project, to_uint8
from ..tiling import Bounds, tile_bounds, tile_size_deg

FEATHER_STEP_M = 4.0  # feather lattice: the pixel size doubled while it stays <= this
FOOTPRINT_SLOP_M = 500.0  # footprints are dilated by one coarse overview pixel (~80 m for NAIP); generous


def pixel_grid(z: int, x: int, y: int):
    w, _s, e, n = tile_bounds(z, x, y)
    d = (e - w) / TILE_PX
    k = np.arange(TILE_PX) + 0.5
    return np.meshgrid(w + k * d, n - k * d), d


def feather_spacing_deg(z: int) -> float:
    d = tile_size_deg(z) / TILE_PX
    while 2 * d * M_PER_DEG <= FEATHER_STEP_M:
        d *= 2
    return d


def feather_margin_deg(z: int, feather_m: float) -> float:
    d = feather_spacing_deg(z)
    return (math.ceil(feather_m / M_PER_DEG / d) + 2) * d


def query_bounds(z: int, x: int, y: int, feather_m: float = 0.0) -> Bounds:
    w, _s, e, n = tile_bounds(z, x, y)
    d = (e - w) / TILE_PX + (feather_margin_deg(z, feather_m) if feather_m else 0.0)
    return (w - d, _s - d, e + d, n + d)


def source_of(entry) -> str:
    return entry.qid.split("/", 1)[0]


def sample_entries(entries, lon: np.ndarray, lat: np.ndarray, target_m: float):
    """Raw band values merged first-valid-wins in `entries` order: (values (3, ...) NaN where none, valid, index of
    the entry each value came from (-1 none))."""
    vals = np.full((3, *lon.shape), np.nan)
    ok = np.zeros(lon.shape, bool)
    which = np.full(lon.shape, -1, np.int32)
    for k, e in enumerate(entries):
        if ok.all():
            break
        slon, slat = e.transform.to_source_geographic(lon, lat)
        px, py = project(e.raster, slon, slat)
        v, good = e.raster.sample(px, py, target_m)
        take = good & ~ok
        if take.any():
            vals[:, take] = v[:3][:, take]
            which[take] = k
            ok |= take
    return vals, ok, which


def decode_entries(entries, vals, which, lon, lat, balance=None) -> np.ndarray:
    """uint8 RGB: the balance's colour match for its target source, each raster's own decoder otherwise; 0 where no
    entry."""
    rgb = np.zeros((3, *which.shape), np.uint8)
    for k, e in enumerate(entries):
        m = which == k
        if not m.any():
            continue
        if balance is not None and source_of(e) == balance.target:
            rgb[:, m] = balance.apply(vals[:, m], lon[m], lat[m])
        else:
            rgb[:, m] = to_uint8(e.raster, vals[:, m])
    return rgb


def feather_weight(z: int, x: int, y: int, ref_entries, feather_m: float) -> np.ndarray:
    """Per pixel, clip(distance to the reference's valid-data edge / feather_m, 0, 1), from the reference's valid mask
    on a lattice over the tile plus a margin (as terrain's feather; degrees of latitude on both axes)."""
    w, _s, e, n = tile_bounds(z, x, y)
    d = feather_spacing_deg(z)
    m = round(feather_margin_deg(z, feather_m) / d)
    k = np.arange(-m, round((e - w) / d) + m) + 0.5
    lon, lat = np.meshgrid(w + k * d, n - k * d)
    _, ok, _ = sample_entries(ref_entries, lon, lat, d * M_PER_DEG)
    if ok.all():
        return np.ones((TILE_PX, TILE_PX))
    if not ok.any():
        return np.zeros((TILE_PX, TILE_PX))
    wl = np.clip(distance_transform_edt(ok) * d / (feather_m / M_PER_DEG), 0.0, 1.0)
    f = (np.arange(TILE_PX) + 0.5) * (e - w) / TILE_PX / d - 0.5 + m  # pixel centres in lattice index units
    fy, fx = np.meshgrid(f, f, indexing="ij")
    return map_coordinates(wl, [fy, fx], order=1, mode="nearest")


def ref_interior(footprints, feather_m: float):
    """The reference's footprints shrunk by the feather plus FOOTPRINT_SLOP_M: a leaf inside it is surely all
    reference beyond any feather."""
    g = shapely.union_all(list(footprints)).buffer(-(feather_m + FOOTPRINT_SLOP_M) / M_PER_DEG)
    shapely.prepare(g)
    return g


def inside(interior, z: int, x: int, y: int) -> bool:
    if interior is None or interior.is_empty:
        return False
    return bool(shapely.contains(interior, shapely.box(*tile_bounds(z, x, y))))


def leaf_rgb(z: int, x: int, y: int, entries, balance=None, interior=None) -> np.ndarray:
    (lon, lat), d = pixel_grid(z, x, y)
    tm = d * M_PER_DEG
    if balance is None:
        vals, _, which = sample_entries(entries, lon, lat, tm)
        return decode_entries(entries, vals, which, lon, lat)
    ref = [e for e in entries if source_of(e) == balance.reference]
    rest = [e for e in entries if source_of(e) != balance.reference]
    rv, rok, rw = sample_entries(ref, lon, lat, tm)
    ref_rgb = decode_entries(ref, rv, rw, lon, lat, balance)
    if rok.all() and inside(interior, z, x, y):
        return ref_rgb
    wt = feather_weight(z, x, y, ref, balance.feather_m) if ref else np.zeros(lon.shape)
    wt = np.where(rok, wt, 0.0)
    if (wt >= 1.0).all():
        return ref_rgb
    sv, _, sw = sample_entries(rest, lon, lat, tm)
    rest_rgb = decode_entries(rest, sv, sw, lon, lat, balance)
    return np.rint(wt * ref_rgb + (1.0 - wt) * rest_rgb).astype(np.uint8)


def encode_jpeg(rgb: np.ndarray, quality: int) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(np.ascontiguousarray(np.moveaxis(rgb, 0, -1))).save(
        buf, "JPEG", quality=quality, subsampling=2, optimize=False, progressive=False
    )
    return buf.getvalue()


def leaf_tile(z: int, x: int, y: int, entries, quality: int, balance=None, interior=None) -> bytes:
    return encode_jpeg(leaf_rgb(z, x, y, entries, balance, interior), quality)


def parent_tile(children: dict[tuple[int, int], bytes], quality: int) -> bytes:
    if len(children) != 4:
        raise ValueError(f"a parent needs its four children, got {sorted(children)}")
    mosaic = np.zeros((2 * TILE_PX, 2 * TILE_PX, 3), np.uint16)
    for (dx, dy), data in children.items():
        r0, c0 = (1 - dy) * TILE_PX, dx * TILE_PX
        mosaic[r0 : r0 + TILE_PX, c0 : c0 + TILE_PX] = np.asarray(Image.open(io.BytesIO(data)).convert("RGB"))
    small = (mosaic[0::2, 0::2] + mosaic[1::2, 0::2] + mosaic[0::2, 1::2] + mosaic[1::2, 1::2] + 2) // 4
    return encode_jpeg(np.moveaxis(small.astype(np.uint8), -1, 0), quality)
