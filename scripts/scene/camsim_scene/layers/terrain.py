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
from ..water import class_codes

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


@dataclass(frozen=True)
class TidalMask:
    """3DEP's hydro-flattened plate over tidal water falls through to topobathy (spec Part A): a sample of a
    `sources` group is invalid where WorldCover is water, its source value is <= max_raw_m and a `topobathy` group
    is valid there."""

    classes: tuple  # WorldCover SourceRasters (water.class_codes)
    water_class: int
    max_raw_m: float
    sources: frozenset
    topobathy: frozenset

    @classmethod
    def from_settings(cls, s: dict, classes: tuple) -> TidalMask:
        return cls(
            tuple(classes), int(s["class"]), float(s["max_navd88_m"]), frozenset(s["sources"]), frozenset(s["topobathy"])
        )


def _source_id(group) -> str:
    return group[0].qid.split("/", 1)[0]


def _max_raw(group, tidal: TidalMask | None) -> float | None:
    return tidal.max_raw_m if tidal is not None and _source_id(group) in tidal.sources else None


def _apply_tidal(groups, results, tidal: TidalMask | None, z: int, lon, lat) -> list:
    """results[i] = (h, valid[, low]); returns [(h, valid)] with tidal samples of masked groups made invalid."""
    if tidal is None:
        return [r[:2] for r in results]
    topo = np.zeros(lon.shape, bool)
    for g, r in zip(groups, results):
        if _source_id(g) in tidal.topobathy:
            topo |= r[1]
    out = []
    for r in results:
        h, valid = r[0], r[1]
        if len(r) == 3:
            cand = r[2] & topo
            if cand.any():
                target_m = spacing_deg(z) * M_PER_DEG
                water = class_codes(tidal.classes, lon[cand], lat[cand], target_m) == tidal.water_class
                idx = np.nonzero(cand)  # works for the 2-D tile grid and 1-D verify points
                valid = valid.copy()
                valid[tuple(i[water] for i in idx)] = False
        out.append((h, valid))
    return out


def _groups(entries) -> list[list]:
    return [list(g) for _, g in groupby(entries, key=lambda e: e.order[:3])]


def sample_group(
    group,
    z: int,
    lon: np.ndarray,
    lat: np.ndarray,
    exact_offsets: bool = False,
    lattice_lon: np.ndarray | None = None,
    max_raw: float | None = None,
):
    """Ellipsoid heights from one merge group (its assets first-valid-wins) and where they are valid. `lattice_lon`
    is `lon` unwrapped (contiguous across the antimeridian) for the offset lattice; it defaults to `lon`. Returns
    (h, valid), or (h, valid, low) with `max_raw`: low = valid and the source value (before the datum offset) is at
    most max_raw."""
    lattice_lon = lon if lattice_lon is None else lattice_lon
    h = np.full(lon.shape, np.nan)
    valid = np.zeros(lon.shape, bool)
    low = np.zeros(lon.shape, bool) if max_raw is not None else None
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
        if low is not None:
            low |= take & (vals[0] <= max_raw)
        valid |= take
    return (h, valid) if low is None else (h, valid, low)


@dataclass
class TileGrid:
    lon: np.ndarray
    lat: np.ndarray
    h: np.ndarray
    seam: np.ndarray  # True inside a feather band (excluded from verify's height comparison)


def tile_grid(z: int, x: int, y: int, entries, tidal: TidalMask | None = None) -> TileGrid:
    ulon, lat = _unwrapped_grid(z, x, y)
    lon = _wrap(ulon)
    groups = _groups(entries)
    if not groups:
        raise TerrainError(f"terrain {z}/{x}/{y}: no source covers the tile")
    results = [sample_group(g, z, lon, lat, lattice_lon=ulon, max_raw=_max_raw(g, tidal)) for g in groups]
    results = _apply_tidal(groups, results, tidal, z, lon, lat)
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


def build_tile(z: int, x: int, y: int, entries, tidal: TidalMask | None = None) -> bytes:
    from pydelatin import Delatin

    g = tile_grid(z, x, y, entries, tidal)
    tin = Delatin(np.ascontiguousarray(g.h, dtype=np.float32), width=GRID, height=GRID, max_error=max_error(z))
    v = np.rint(np.asarray(tin.vertices)[:, :2]).astype(np.int64)
    col, yup = v[:, 0], v[:, 1]  # pydelatin's y counts up from the last row (south)
    w, s, e, n = tile_bounds(z, x, y)
    d = spacing_deg(z)
    lon, lat = w + col * d, s + yup * d
    heights = g.h[GRID - 1 - yup, col]  # float64 grid values, not pydelatin's float32 copy
    return qmesh.gzip_tile(qmesh.encode(lon, lat, heights, np.asarray(tin.triangles, np.int64), (w, s, e, n)))


def point_heights(z: int, lon: np.ndarray, lat: np.ndarray, entries, tidal: TidalMask | None = None) -> np.ndarray:
    """Highest-priority valid height at arbitrary points, vertical offsets evaluated exactly (verify --deep). A raster
    geoid is read at the zoom's lattice spacing, as the build read it, not at the points' (irregular, dense) spacing:
    memory stays bounded at coarse zooms."""
    out = np.full(np.shape(lon), np.nan)
    groups = _groups(entries)
    results = [sample_group(g, z, lon, lat, exact_offsets=True, max_raw=_max_raw(g, tidal)) for g in groups]
    for h, ok in _apply_tidal(groups, results, tidal, z, lon, lat):
        out = np.where(np.isnan(out) & ok, h, out)
    return out
