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


def leaf_rgb(z: int, x: int, y: int, entries) -> np.ndarray:
    (lon, lat), d = pixel_grid(z, x, y)
    rgb = np.zeros((3, TILE_PX, TILE_PX), np.uint8)
    filled = np.zeros((TILE_PX, TILE_PX), bool)
    for e in entries:
        if filled.all():
            break
        slon, slat = e.transform.to_source_geographic(lon, lat)
        px, py = project(e.raster, slon, slat)
        vals, ok = e.raster.sample(px, py, d * M_PER_DEG)
        take = ok & ~filled
        if take.any():
            rgb[:, take] = to_uint8(e.raster, vals)[:, take]
            filled |= take
    return rgb


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
