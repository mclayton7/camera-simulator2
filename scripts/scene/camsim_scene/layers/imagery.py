"""Imagery tile jobs. Leaves (tiles without children) sample their sources at pixel centres in priority order,
nodata falling through; parents are a 2 x 2 box filter of their four children (always four: complete siblings).
JPEG q85 4:2:0 via Pillow."""

from __future__ import annotations

import io

import numpy as np
from PIL import Image

from ..config import TILE_PX
from ..sources.base import M_PER_DEG, project, to_uint8
from ..tiling import Bounds, tile_bounds


def pixel_grid(z: int, x: int, y: int):
    w, _s, e, n = tile_bounds(z, x, y)
    d = (e - w) / TILE_PX
    k = np.arange(TILE_PX) + 0.5
    return np.meshgrid(w + k * d, n - k * d), d


def query_bounds(z: int, x: int, y: int) -> Bounds:
    w, _s, e, n = tile_bounds(z, x, y)
    d = (e - w) / TILE_PX
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


def leaf_rgb(z: int, x: int, y: int, entries) -> np.ndarray:
    (lon, lat), d = pixel_grid(z, x, y)
    vals, _, which = sample_entries(entries, lon, lat, d * M_PER_DEG)
    return decode_entries(entries, vals, which, lon, lat)


def encode_jpeg(rgb: np.ndarray, quality: int) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(np.ascontiguousarray(np.moveaxis(rgb, 0, -1))).save(
        buf, "JPEG", quality=quality, subsampling=2, optimize=False, progressive=False
    )
    return buf.getvalue()


def leaf_tile(z: int, x: int, y: int, entries, quality: int) -> bytes:
    return encode_jpeg(leaf_rgb(z, x, y, entries), quality)


def parent_tile(children: dict[tuple[int, int], bytes], quality: int) -> bytes:
    if len(children) != 4:
        raise ValueError(f"a parent needs its four children, got {sorted(children)}")
    mosaic = np.zeros((2 * TILE_PX, 2 * TILE_PX, 3), np.uint16)
    for (dx, dy), data in children.items():
        r0, c0 = (1 - dy) * TILE_PX, dx * TILE_PX
        mosaic[r0 : r0 + TILE_PX, c0 : c0 + TILE_PX] = np.asarray(Image.open(io.BytesIO(data)).convert("RGB"))
    small = (mosaic[0::2, 0::2] + mosaic[1::2, 0::2] + mosaic[0::2, 1::2] + mosaic[1::2, 1::2] + 2) // 4
    return encode_jpeg(np.moveaxis(small.astype(np.uint8), -1, 0), quality)
