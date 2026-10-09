"""layers/imagery.py's leaf_rgb as it was before the NAIP-edge work (2026-10-09): the reference that balance = false
must reproduce byte for byte. Do not edit (pixel_grid is inlined so changes to layers/imagery.py can't reach it)."""

import numpy as np

from camsim_scene.config import TILE_PX
from camsim_scene.sources.base import M_PER_DEG, project, to_uint8
from camsim_scene.tiling import tile_bounds


def pixel_grid(z, x, y):
    w, _s, e, n = tile_bounds(z, x, y)
    d = (e - w) / TILE_PX
    k = np.arange(TILE_PX) + 0.5
    return np.meshgrid(w + k * d, n - k * d), d


def leaf_rgb(z, x, y, entries):
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
