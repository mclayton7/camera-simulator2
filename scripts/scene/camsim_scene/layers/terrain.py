"""Terrain tile job: sample grid -> per-source windows -> ellipsoid heights -> priority + feather -> TIN -> writer.

Neighbouring tiles agree on their shared edge exactly: they sample the same lon/lat there (tile edges and the
sample spacing are binary fractions of a degree), choose overview levels from the zoom alone, share vertical-offset
lattice nodes, and the feather's distance field sees at least 30 m beyond every edge (the margin)."""

from __future__ import annotations

import math
from dataclasses import dataclass
from itertools import groupby

import numpy as np
from scipy.ndimage import distance_transform_edt

from .. import qmesh
from ..config import FEATHER_M
from ..config import TERRAIN_GRID as GRID
from ..datum import lattice_step_deg, offset_lattice
from ..sources.base import M_PER_DEG, project
from ..tiling import Bounds, tile_bounds, tile_size_deg

MIN_MARGIN = 32
FEATHER_DEG = FEATHER_M / M_PER_DEG  # measured in degrees of latitude on both axes (Decision 8)


class TerrainError(Exception):
    pass


def max_error(z: int) -> float:
    """A quarter of Cesium's assumed geometric error at level z, never below 0.1 m."""
    return max(0.1, 0.25 * 77067.0 / (1 << z))


def spacing_deg(z: int) -> float:
    return tile_size_deg(z) / (GRID - 1)


def margin(z: int) -> int:
    return max(MIN_MARGIN, math.ceil(FEATHER_DEG / spacing_deg(z)) + 2)


def query_bounds(z: int, x: int, y: int) -> Bounds:
    w, s, e, n = tile_bounds(z, x, y)
    m = margin(z) * spacing_deg(z)
    return (w - m, s - m, e + m, n + m)


def _unwrapped_grid(z: int, x: int, y: int) -> tuple[np.ndarray, np.ndarray]:
    """The sample grid with contiguous longitudes (may leave [-180, 180] by the margin at the antimeridian)."""
    w, _s, _e, n = tile_bounds(z, x, y)
    d, m = spacing_deg(z), margin(z)
    k = np.arange(-m, GRID + m)
    return np.meshgrid(w + k * d, np.clip(n - k * d, -90.0, 90.0))


def _wrap(lon: np.ndarray) -> np.ndarray:
    return np.where(lon > 180.0, lon - 360.0, np.where(lon < -180.0, lon + 360.0, lon))


def sample_grid(z: int, x: int, y: int) -> tuple[np.ndarray, np.ndarray]:
    ulon, lat = _unwrapped_grid(z, x, y)
    return _wrap(ulon), lat


def _groups(entries) -> list[list]:
    return [list(g) for _, g in groupby(entries, key=lambda e: e.order[:3])]


def sample_group(
    group, z: int, lon: np.ndarray, lat: np.ndarray, exact_offsets: bool = False, lattice_lon: np.ndarray | None = None
):
    """Ellipsoid heights from one merge group (its assets first-valid-wins) and where they are valid. `lattice_lon`
    is `lon` unwrapped (contiguous across the antimeridian) for the offset lattice; it defaults to `lon`."""
    lattice_lon = lon if lattice_lon is None else lattice_lon
    h = np.full(lon.shape, np.nan)
    valid = np.zeros(lon.shape, bool)
    target_m = spacing_deg(z) * M_PER_DEG
    for e in group:
        need = ~valid
        if not need.any():
            break
        slon, slat = e.transform.to_source_geographic(lon, lat)
        px, py = project(e.raster, slon, slat)
        vals, ok = e.raster.sample(px, py, target_m)
        take = ok & need
        if not take.any():
            continue
        off = (
            e.transform.vertical_offset(slon, slat, lattice_step_deg(z) * M_PER_DEG)
            if exact_offsets
            else offset_lattice(e.transform, z, lattice_lon, lat)
        )
        h[take] = vals[0][take] + off[take]
        valid |= take
    return h, valid


@dataclass
class TileGrid:
    lon: np.ndarray
    lat: np.ndarray
    h: np.ndarray
    seam: np.ndarray  # True inside a feather band (excluded from verify's height comparison)


def tile_grid(z: int, x: int, y: int, entries) -> TileGrid:
    ulon, lat = _unwrapped_grid(z, x, y)
    lon = _wrap(ulon)
    groups = _groups(entries)
    if not groups:
        raise TerrainError(f"terrain {z}/{x}/{y}: no source covers the tile")
    results = [sample_group(g, z, lon, lat, lattice_lon=ulon) for g in groups]
    h, base_ok = results[-1]
    if not base_ok.all():
        raise TerrainError(f"terrain {z}/{x}/{y}: the lowest-priority source ({groups[-1][0].qid}) has gaps")
    h = h.copy()
    seam = np.zeros(lon.shape, bool)
    d = spacing_deg(z)
    for gh, gv in reversed(results[:-1]):
        if not gv.any():
            continue
        w = np.ones(lon.shape) if gv.all() else np.clip(distance_transform_edt(gv) * d / FEATHER_DEG, 0.0, 1.0)
        h = np.where(gv, w * np.nan_to_num(gh) + (1.0 - w) * h, h)
        seam |= gv & (w < 1.0)
    m = margin(z)
    inner = slice(m, m + GRID)
    return TileGrid(lon[inner, inner], lat[inner, inner], h[inner, inner], seam[inner, inner])


def build_tile(z: int, x: int, y: int, entries) -> bytes:
    from pydelatin import Delatin

    g = tile_grid(z, x, y, entries)
    tin = Delatin(np.ascontiguousarray(g.h, dtype=np.float32), width=GRID, height=GRID, max_error=max_error(z))
    v = np.rint(np.asarray(tin.vertices)[:, :2]).astype(np.int64)
    col, yup = v[:, 0], v[:, 1]  # pydelatin's y counts up from the last row (south)
    w, s, e, n = tile_bounds(z, x, y)
    d = spacing_deg(z)
    lon, lat = w + col * d, s + yup * d
    heights = g.h[GRID - 1 - yup, col]  # float64 grid values, not pydelatin's float32 copy
    return qmesh.gzip_tile(qmesh.encode(lon, lat, heights, np.asarray(tin.triangles, np.int64), (w, s, e, n)))


def point_heights(z: int, lon: np.ndarray, lat: np.ndarray, entries) -> np.ndarray:
    """Highest-priority valid height at arbitrary points, vertical offsets evaluated exactly (verify --deep). A raster
    geoid is read at the zoom's lattice spacing, as the build read it, not at the points' (irregular, dense) spacing:
    memory stays bounded at coarse zooms."""
    out = np.full(np.shape(lon), np.nan)
    for g in _groups(entries):
        need = np.isnan(out)
        if not need.any():
            break
        h, ok = sample_group(g, z, lon, lat, exact_offsets=True)
        out = np.where(need & ok, h, out)
    return out
