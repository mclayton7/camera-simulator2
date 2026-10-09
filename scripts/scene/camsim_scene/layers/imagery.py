"""Imagery tile jobs. Leaves (tiles without children) sample their sources at pixel centres in priority order,
nodata falling through; parents are a 2 x 2 box filter of their four children (always four: complete siblings).
JPEG q85 4:2:0 via Pillow. With a colour match (balance.py), the target source is decoded through it and the
reference (NAIP) fades into the next source over `feather_m` inside its valid-data edge. With a water mask (water.py),
the reference is used only within `buffer_m` of land and the colour match fades out to the target's own decode over
`fade_m` of water."""

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
from ..water import WATER, any_water, class_codes

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


def decode_entries(entries, vals, which, lon, lat, balance=None, land=None) -> np.ndarray:
    """uint8 RGB: the balance's colour match for its target source (blended toward the source's own decoder by
    1 - land where `land` is given), each raster's own decoder otherwise; 0 where no entry."""
    rgb = np.zeros((3, *which.shape), np.uint8)
    for k, e in enumerate(entries):
        m = which == k
        if not m.any():
            continue
        if balance is not None and source_of(e) == balance.target:
            v = balance.apply(vals[:, m], lon[m], lat[m])
            if land is not None:
                lw = land[m]
                v = np.rint(lw * v + (1.0 - lw) * to_uint8(e.raster, vals[:, m])).astype(np.uint8)
            rgb[:, m] = v
        else:
            rgb[:, m] = to_uint8(e.raster, vals[:, m])
    return rgb


def lattice(z: int, x: int, y: int, margin_deg: float):
    """Feather-lattice nodes (feather_spacing_deg, aligned across tiles) over the tile plus margin_deg: lon, lat, the
    spacing and the margin in nodes."""
    w, _s, e, n = tile_bounds(z, x, y)
    d = feather_spacing_deg(z)
    m = round(margin_deg / d)
    k = np.arange(-m, round((e - w) / d) + m) + 0.5
    lon, lat = np.meshgrid(w + k * d, n - k * d)
    return lon, lat, d, m


def to_pixels(z: int, x: int, y: int, grid: np.ndarray, d: float, m: int) -> np.ndarray:
    """A lattice(z, x, y, ...) grid, bilinear at the tile's pixel centres."""
    w, _s, e, _n = tile_bounds(z, x, y)
    f = (np.arange(TILE_PX) + 0.5) * (e - w) / TILE_PX / d - 0.5 + m  # pixel centres in lattice index units
    fy, fx = np.meshgrid(f, f, indexing="ij")
    return map_coordinates(grid, [fy, fx], order=1, mode="nearest")


def feather_weight(z: int, x: int, y: int, ref_entries, feather_m: float, allow=None) -> np.ndarray:
    """Per pixel, clip(distance to the reference's valid-data edge / feather_m, 0, 1), from the reference's valid mask
    on a lattice over the tile plus a margin (as terrain's feather; degrees of latitude on both axes). `allow` (bool,
    that lattice's shape): where the reference may be used at all (the water buffer); its edge ramps the same way."""
    lon, lat, d, m = lattice(z, x, y, feather_margin_deg(z, feather_m))
    _, ok, _ = sample_entries(ref_entries, lon, lat, d * M_PER_DEG)
    if allow is not None:
        ok &= allow
    if ok.all():
        return np.ones((TILE_PX, TILE_PX))
    if not ok.any():
        return np.zeros((TILE_PX, TILE_PX))
    wl = np.clip(distance_transform_edt(ok) * d / (feather_m / M_PER_DEG), 0.0, 1.0)
    return to_pixels(z, x, y, wl, d, m)


def land_distance(z: int, x: int, y: int, water, feather_m: float) -> np.ndarray | None:
    """Per node of the feather lattice (tile + feather margin): metres to the nearest node that isn't WorldCover water
    (0 on land; degrees of latitude on both axes, as the feather), from class codes on that lattice grown by
    water.reach_m; inf everywhere when every node is water. None when no node of the feather lattice is water."""
    d = feather_spacing_deg(z)
    pad = math.ceil(water.reach_m / M_PER_DEG / d) + 2
    margin = feather_margin_deg(z, feather_m) + pad * d
    w, s, e, n = tile_bounds(z, x, y)
    if not any_water(water.classes, (w - margin, s - margin, e + margin, n + margin)):
        return None
    lon, lat, d, _ = lattice(z, x, y, margin)
    wet = class_codes(water.classes, lon, lat) == WATER
    inner = (slice(pad, -pad), slice(pad, -pad))
    if not wet[inner].any():
        return None
    if wet.all():
        return np.full(wet[inner].shape, np.inf)
    return distance_transform_edt(wet)[inner] * (d * M_PER_DEG)


def pixel_distance(z: int, x: int, y: int, dist: np.ndarray, feather_m: float) -> np.ndarray:
    """land_distance at the tile's pixel centres (bilinear)."""
    if np.isinf(dist).all():
        return np.full((TILE_PX, TILE_PX), np.inf)
    d = feather_spacing_deg(z)
    return to_pixels(z, x, y, dist, d, round(feather_margin_deg(z, feather_m) / d))


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


def leaf_rgb(z: int, x: int, y: int, entries, balance=None, interior=None, water=None) -> np.ndarray:
    (lon, lat), d = pixel_grid(z, x, y)
    tm = d * M_PER_DEG
    if balance is None:
        vals, _, which = sample_entries(entries, lon, lat, tm)
        return decode_entries(entries, vals, which, lon, lat)
    ref = [e for e in entries if source_of(e) == balance.reference]
    rest = [e for e in entries if source_of(e) != balance.reference]
    rv, rok, rw = sample_entries(ref, lon, lat, tm)
    ref_rgb = decode_entries(ref, rv, rw, lon, lat, balance)
    dist = land_distance(z, x, y, water, balance.feather_m) if water is not None else None
    allow = (dist <= water.buffer_m) if dist is not None and water.buffer_m > 0 else None
    if rok.all() and inside(interior, z, x, y) and (allow is None or allow.all()):
        return ref_rgb
    dpx = None if dist is None else pixel_distance(z, x, y, dist, balance.feather_m)
    wt = feather_weight(z, x, y, ref, balance.feather_m, allow) if ref else np.zeros(lon.shape)
    if allow is not None:
        wt = np.where(dpx <= water.buffer_m, wt, 0.0)  # beyond the water buffer: never the reference
    if (np.where(rok, wt, 0.0) >= 1.0).all():
        return ref_rgb
    sv, sok, sw = sample_entries(rest, lon, lat, tm)
    wt = np.where(sok, wt, 1.0)  # nothing behind the reference here: keep it, never fade it toward black
    wt = np.where(rok, wt, 0.0)
    if (wt >= 1.0).all():
        return ref_rgb
    land = None if dpx is None else np.clip(1.0 - dpx / water.fade_m, 0.0, 1.0)
    rest_rgb = decode_entries(rest, sv, sw, lon, lat, balance, land)
    return np.rint(wt * ref_rgb + (1.0 - wt) * rest_rgb).astype(np.uint8)


def encode_jpeg(rgb: np.ndarray, quality: int) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(np.ascontiguousarray(np.moveaxis(rgb, 0, -1))).save(
        buf, "JPEG", quality=quality, subsampling=2, optimize=False, progressive=False
    )
    return buf.getvalue()


def leaf_tile(z: int, x: int, y: int, entries, quality: int, balance=None, interior=None, water=None) -> bytes:
    return encode_jpeg(leaf_rgb(z, x, y, entries, balance, interior, water), quality)


def parent_tile(children: dict[tuple[int, int], bytes], quality: int) -> bytes:
    if len(children) != 4:
        raise ValueError(f"a parent needs its four children, got {sorted(children)}")
    mosaic = np.zeros((2 * TILE_PX, 2 * TILE_PX, 3), np.uint16)
    for (dx, dy), data in children.items():
        r0, c0 = (1 - dy) * TILE_PX, dx * TILE_PX
        mosaic[r0 : r0 + TILE_PX, c0 : c0 + TILE_PX] = np.asarray(Image.open(io.BytesIO(data)).convert("RGB"))
    small = (mosaic[0::2, 0::2] + mosaic[1::2, 0::2] + mosaic[0::2, 1::2] + mosaic[1::2, 1::2] + 2) // 4
    return encode_jpeg(np.moveaxis(small.astype(np.uint8), -1, 0), quality)
