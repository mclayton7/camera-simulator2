"""dense_pass path selection (REALISM R3/R4 bench): score straight segments by the tree cover (WorldCover 10) and
built-up land (50) the forward-looking camera sweeps over, from a scene package's land-cover tiles.

The building signal is built-up land, not R3's buildings layer, so the path does not depend on R3. The camera
footprint is approximated by the strip it sweeps: from near_m to length + far_m along the track, +-half_width_m across.

Usage (library): from bench.dense_path import LandCover, class_grid, candidates
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image

TREE = 10
BUILT = 50
M_PER_DEG = 111_320.0
TILE_DEG = 0.05  # camsim-landcover-1
TILE_PX = 600


def lc_tile_filename(i: int, j: int) -> str:
    return f"{i * TILE_DEG:+.2f}_{j * TILE_DEG:+.2f}.png"


class LandCover:
    """Land-cover codes at points; a missing tile reads as 0."""

    def __init__(self, root: Path) -> None:
        self.root = Path(root)
        self._tiles: dict[tuple[int, int], np.ndarray | None] = {}

    def _tile(self, i: int, j: int) -> np.ndarray | None:
        if (i, j) not in self._tiles:
            p = self.root / lc_tile_filename(i, j)
            self._tiles[(i, j)] = np.asarray(Image.open(p)) if p.exists() else None
        return self._tiles[(i, j)]

    def codes(self, lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
        lon, lat = np.asarray(lon, float), np.asarray(lat, float)
        # round(x, 9) before floor, as the scene tooling's tile index (layers/landcover.py): a point on a tile edge
        # whose division lands a hair under the integer belongs to the tile above it, not the one below.
        ti = np.floor(np.round(lat / TILE_DEG, 9)).astype(np.int64)
        tj = np.floor(np.round(lon / TILE_DEG, 9)).astype(np.int64)
        out = np.zeros(lon.shape, np.uint8)
        for i, j in set(zip(ti.ravel().tolist(), tj.ravel().tolist())):
            a = self._tile(i, j)
            if a is None:
                continue
            m = (ti == i) & (tj == j)
            col = np.clip(np.floor((lon[m] - j * TILE_DEG) / TILE_DEG * TILE_PX).astype(np.int64), 0, TILE_PX - 1)
            row = np.clip(np.floor(((i + 1) * TILE_DEG - lat[m]) / TILE_DEG * TILE_PX).astype(np.int64), 0, TILE_PX - 1)
            out[m] = a[row, col]
        return out


@dataclass
class Grid:
    origin_lon: float  # south-west corner
    origin_lat: float
    cell_m: float
    tree: np.ndarray  # [row north of origin, col east of origin], fraction 0..1
    built: np.ndarray

    def m_per_deg_lon(self) -> float:
        return M_PER_DEG * math.cos(math.radians(self.origin_lat))


def xy_to_lonlat(grid: Grid, x: float, y: float) -> tuple[float, float]:
    return grid.origin_lon + x / grid.m_per_deg_lon(), grid.origin_lat + y / M_PER_DEG


def class_grid(
    lc: LandCover, bbox: tuple[float, float, float, float], cell_m: float = 100.0, step_m: float = 20.0
) -> Grid:
    w, s, e, n = bbox
    kx = M_PER_DEG * math.cos(math.radians(s))
    cols = int(round((e - w) * kx / cell_m))
    rows = int(round((n - s) * M_PER_DEG / cell_m))
    k = max(1, int(round(cell_m / step_m)))
    sub = (np.arange(k) + 0.5) * cell_m / k
    tree = np.zeros((rows, cols))
    built = np.zeros((rows, cols))
    for r in range(rows):  # one row of cells at a time keeps memory flat
        ys = r * cell_m + sub
        xs = (np.arange(cols)[:, None] * cell_m + sub[None, :]).ravel()
        X, Y = np.meshgrid(xs, ys)
        c = lc.codes(w + X / kx, s + Y / M_PER_DEG).reshape(k, cols, k)
        tree[r] = (c == TREE).mean(axis=(0, 2))
        built[r] = (c == BUILT).mean(axis=(0, 2))
    return Grid(w, s, cell_m, tree, built)


def strip_points(
    x0: float,
    y0: float,
    bearing_deg: float,
    length_m: float,
    near_m: float = 400.0,
    far_m: float = 2000.0,
    half_width_m: float = 800.0,
    step_m: float = 100.0,
) -> tuple[np.ndarray, np.ndarray]:
    b = math.radians(bearing_deg)
    fx, fy = math.sin(b), math.cos(b)  # forward (bearing from north, clockwise)
    rx, ry = fy, -fx  # right
    along = np.arange(near_m, length_m + far_m + step_m / 2, step_m)
    across = np.arange(-half_width_m, half_width_m + step_m / 2, step_m) if half_width_m > 0 else np.array([0.0])
    A, C = np.meshgrid(along, across)
    return (x0 + A * fx + C * rx).ravel(), (y0 + A * fy + C * ry).ravel()


def _sample(grid: Grid, a: np.ndarray, x: np.ndarray, y: np.ndarray) -> float:
    c = np.floor(x / grid.cell_m).astype(np.int64)
    r = np.floor(y / grid.cell_m).astype(np.int64)
    ok = (r >= 0) & (r < a.shape[0]) & (c >= 0) & (c < a.shape[1])
    return float(a[r[ok], c[ok]].sum())


@dataclass
class Candidate:
    x0: float
    y0: float
    bearing_deg: float
    tree: float
    built: float
    score: float = 0.0


def candidates(
    grid: Grid,
    length_m: float = 9000.0,
    start_step_m: float = 1000.0,
    bearing_step_deg: float = 15.0,
    margin_m: float = 500.0,
) -> list[Candidate]:
    """Every straight segment whose start and end stay margin_m inside the grid, scored by the normalised sum of the
    tree and built-up fractions in its swept strip; segments missing either class are dropped. Best first; ties keep
    generation order (start south-west first, bearing ascending)."""
    ext_x = grid.tree.shape[1] * grid.cell_m
    ext_y = grid.tree.shape[0] * grid.cell_m
    out: list[Candidate] = []
    for y0 in np.arange(margin_m, ext_y - margin_m + 1e-6, start_step_m):
        for x0 in np.arange(margin_m, ext_x - margin_m + 1e-6, start_step_m):
            for bd in np.arange(0.0, 360.0, bearing_step_deg):
                b = math.radians(bd)
                x1, y1 = x0 + length_m * math.sin(b), y0 + length_m * math.cos(b)
                if not (margin_m <= x1 <= ext_x - margin_m and margin_m <= y1 <= ext_y - margin_m):
                    continue
                xs, ys = strip_points(float(x0), float(y0), float(bd), length_m)
                t, bu = _sample(grid, grid.tree, xs, ys), _sample(grid, grid.built, xs, ys)
                if t > 0 and bu > 0:
                    out.append(Candidate(float(x0), float(y0), float(bd), t, bu))
    if not out:
        return out
    tmax, bmax = max(c.tree for c in out), max(c.built for c in out)
    for c in out:
        c.score = c.tree / tmax + c.built / bmax
    return sorted(out, key=lambda c: -c.score)  # sorted() is stable
