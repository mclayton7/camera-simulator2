"""Sentinel-2 -> NAIP colour match (REALISM R1; docs/superpowers/specs/2026-10-09-naip-edge-design.md).

A per-band tone curve (quantile match, raw Sentinel-2 DN -> NAIP DN) plus a smooth per-band offset on a coarse grid,
fitted once per build (fit_balance) and stored as imagery/balance.json. Every stored value is quantised to an
integer step, so float summation order can't change the file; workers always read the model back from it."""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field

import numpy as np
from scipy.ndimage import distance_transform_edt

from .manifest import canonical_json
from .sources.base import M_PER_DEG

FILE = "balance.json"
FORMAT = 1
RAW_STEP = 0.01  # raw Sentinel-2 DN (reflectance x 1e4): 1e-6 reflectance
DN_STEP = 0.1  # NAIP DN
GRID_STEP = 1e-9  # degrees


def _q(v, step: float) -> np.ndarray:
    return np.rint(np.asarray(v, np.float64) / step).astype(np.int64)


def _r(v: float) -> float:
    return round(float(v) / GRID_STEP) * GRID_STEP


@dataclass(frozen=True)
class Grid:
    """Offset cells: west/south corner, cell size in degrees, counts. Row 0 is the southernmost."""

    west: float
    south: float
    cell_lon: float
    cell_lat: float
    nx: int
    ny: int

    @classmethod
    def covering(cls, bounds, cell_km: float, lat0: float) -> Grid:
        w, s, e, n = bounds
        cell_lat = cell_km * 1000.0 / M_PER_DEG
        cell_lon = cell_lat / math.cos(math.radians(lat0))
        nx = max(1, math.ceil((e - w) / cell_lon))
        ny = max(1, math.ceil((n - s) / cell_lat))
        return cls(_r(w), _r(s), _r(cell_lon), _r(cell_lat), nx, ny)

    def index(self, lon, lat) -> tuple[np.ndarray, np.ndarray]:
        ix = np.floor((np.asarray(lon, np.float64) - self.west) / self.cell_lon).astype(np.int64)
        iy = np.floor((np.asarray(lat, np.float64) - self.south) / self.cell_lat).astype(np.int64)
        return np.clip(ix, 0, self.nx - 1), np.clip(iy, 0, self.ny - 1)

    def to_dict(self) -> dict:
        return {
            "west": self.west,
            "south": self.south,
            "cell_lon": self.cell_lon,
            "cell_lat": self.cell_lat,
            "nx": self.nx,
            "ny": self.ny,
        }

    @classmethod
    def from_dict(cls, d: dict) -> Grid:
        return cls(d["west"], d["south"], d["cell_lon"], d["cell_lat"], int(d["nx"]), int(d["ny"]))


@dataclass
class Balance:
    reference: str
    target: str
    feather_m: float
    xq: list[np.ndarray]  # per band: raw Sentinel-2 DN, strictly increasing
    yq: list[np.ndarray]  # per band: NAIP DN
    grid: Grid
    offsets: np.ndarray  # (3, ny, nx) NAIP DN
    report: dict = field(default_factory=dict)  # fit statistics: build.json and the log, never the file

    @classmethod
    def make(cls, reference, target, feather_m, xq, yq, grid: Grid, offsets, report=None) -> Balance:
        """Quantise everything to the file's steps; drop quantiles whose x ties after rounding (first kept)."""
        xs, ys = [], []
        for x, y in zip(xq, yq):
            xi, yi = _q(x, RAW_STEP), _q(y, DN_STEP)
            keep = np.concatenate([[True], np.diff(xi) > 0])
            xs.append(xi[keep] * RAW_STEP)
            ys.append(yi[keep] * DN_STEP)
        off = _q(offsets, DN_STEP).reshape(3, grid.ny, grid.nx) * DN_STEP
        return cls(reference, target, float(feather_m), xs, ys, grid, off, dict(report or {}))

    def tone(self, raw) -> np.ndarray:
        raw = np.asarray(raw, np.float64)
        return np.stack([np.interp(np.nan_to_num(raw[b]), self.xq[b], self.yq[b]) for b in range(3)])

    def offset_at(self, lon, lat) -> np.ndarray:
        """Bilinear between cell centres, clamped at the grid's edge cells."""
        g = self.grid
        fx = np.clip((np.asarray(lon, np.float64) - g.west) / g.cell_lon - 0.5, 0.0, g.nx - 1.0)
        fy = np.clip((np.asarray(lat, np.float64) - g.south) / g.cell_lat - 0.5, 0.0, g.ny - 1.0)
        x0 = np.minimum(np.floor(fx).astype(np.int64), max(g.nx - 2, 0))
        y0 = np.minimum(np.floor(fy).astype(np.int64), max(g.ny - 2, 0))
        x1, y1 = np.minimum(x0 + 1, g.nx - 1), np.minimum(y0 + 1, g.ny - 1)
        tx, ty = fx - x0, fy - y0
        o = self.offsets
        return (
            o[:, y0, x0] * (1 - tx) * (1 - ty)
            + o[:, y0, x1] * tx * (1 - ty)
            + o[:, y1, x0] * (1 - tx) * ty
            + o[:, y1, x1] * tx * ty
        )

    def apply(self, raw, lon, lat) -> np.ndarray:
        return np.clip(np.rint(self.tone(raw) + self.offset_at(lon, lat)), 0, 255).astype(np.uint8)

    def to_json(self) -> bytes:
        d = {
            "format": FORMAT,
            "reference": self.reference,
            "target": self.target,
            "feather_m": self.feather_m,
            "raw_step": RAW_STEP,
            "dn_step": DN_STEP,
            "tone_raw": [_q(x, RAW_STEP).tolist() for x in self.xq],
            "tone_dn": [_q(y, DN_STEP).tolist() for y in self.yq],
            "grid": self.grid.to_dict(),
            "offsets": _q(self.offsets, DN_STEP).tolist(),
        }
        return canonical_json(d).encode()

    @classmethod
    def from_json(cls, data: bytes) -> Balance:
        d = json.loads(data)
        if d.get("format") != FORMAT:
            raise ValueError(f"balance.json format {d.get('format')!r}, expected {FORMAT}")
        g = Grid.from_dict(d["grid"])
        xs = [np.asarray(v, np.float64) * d["raw_step"] for v in d["tone_raw"]]
        ys = [np.asarray(v, np.float64) * d["dn_step"] for v in d["tone_dn"]]
        off = np.asarray(d["offsets"], np.float64) * d["dn_step"]
        return cls(d["reference"], d["target"], float(d["feather_m"]), xs, ys, g, off)


def cell_medians(resid: np.ndarray, cells: np.ndarray, ncells: int, min_n: int) -> np.ndarray:
    """Per-cell median of each band's residual; NaN for cells with fewer than min_n samples."""
    out = np.full((resid.shape[0], ncells), np.nan)
    order = np.argsort(cells, kind="stable")
    c, r = cells[order], resid[:, order]
    uniq, start, count = np.unique(c, return_index=True, return_counts=True)
    for u, a, n in zip(uniq.tolist(), start.tolist(), count.tolist()):
        if n >= min_n:
            out[:, u] = np.median(r[:, a : a + n], axis=1)
    return out


def fill_offsets(values: np.ndarray, cell_km: float, decay_km: float, median_size: int = 3) -> np.ndarray:
    """median_size x median_size median over fitted cells only, then every unfitted
    cell takes its nearest fitted cell's value times exp(-d / decay_km), d the distance between the cells' centres."""
    fitted = np.isfinite(values[0])
    if not fitted.any():
        return np.zeros_like(values)
    filt = values.copy()
    h = median_size // 2
    for y, x in zip(*np.nonzero(fitted)):
        win = values[:, max(0, y - h) : y + h + 1, max(0, x - h) : x + h + 1].reshape(values.shape[0], -1)
        filt[:, y, x] = np.nanmedian(win, axis=1)
    d, (iy, ix) = distance_transform_edt(~fitted, return_indices=True)
    return filt[:, iy, ix] * np.exp(-d * cell_km / decay_km)
