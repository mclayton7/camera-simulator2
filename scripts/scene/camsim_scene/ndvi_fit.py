"""NAIP -> Sentinel-2 NDVI fit (REALISM R1 chunk 2; docs/superpowers/specs/2026-10-09-ndvi-sun-metadata-design.md).

NAIP NDVI comes from uncalibrated DN. One linear map (gain, offset), least squares on the land lattice the colour
match uses (balance.py), puts it on Sentinel-2's near-reflectance scale; a smooth per-cell offset on top (the median
residual per cell, filtered and filled as the colour match's offsets) removes what one global map leaves near NAIP's
edge, where flight days and quads differ. Stored as ndvi/fit.json in integers (millionths; the grid in
nanodegrees), so float summation order can't change the file; workers read it back from the file."""

from __future__ import annotations

import json
import logging
from dataclasses import dataclass, field

import numpy as np

from .balance import Grid, cell_medians, fill_offsets, offset_at, shared_land_samples
from .manifest import canonical_json
from .tiling import Bounds

log = logging.getLogger(__name__)
FILE = "fit.json"
FORMAT = 2
MICRO = 1_000_000
NANO = 1_000_000_000


@dataclass
class NdviFit:
    """apply(n, lon, lat) = clip(gain * n + offset + cell_offset(lon, lat), -1, 1). cell_offsets_e6: rows south to
    north (row 0 southernmost, as balance.Grid), each west to east."""

    reference: str
    target: str
    gain_e6: int
    offset_e6: int
    grid_e9: tuple[int, int, int, int]  # west, south, cell_lon, cell_lat (degrees x 1e9)
    nx: int
    ny: int
    cell_offsets_e6: tuple[tuple[int, ...], ...]
    report: dict = field(default_factory=dict, compare=False)
    _cells: np.ndarray = field(init=False, compare=False, repr=False)

    def __post_init__(self):
        cells = np.asarray(self.cell_offsets_e6, np.float64)
        if cells.shape != (self.ny, self.nx):
            raise ValueError(f"ndvi fit: cell offsets {cells.shape} != grid {(self.ny, self.nx)}")
        self._cells = (cells / MICRO)[None]

    @classmethod
    def make(cls, reference: str, target: str, gain: float, offset: float, grid: Grid, cells=None) -> NdviFit:
        """Quantise to the file's integers; cells (ny, nx) default to zero."""
        c = np.zeros((grid.ny, grid.nx)) if cells is None else np.asarray(cells, np.float64)
        e9 = tuple(round(v * NANO) for v in (grid.west, grid.south, grid.cell_lon, grid.cell_lat))
        rows = tuple(tuple(r) for r in np.rint(c * MICRO).astype(np.int64).tolist())
        return cls(reference, target, round(gain * MICRO), round(offset * MICRO), e9, grid.nx, grid.ny, rows)

    @property
    def gain(self) -> float:
        return self.gain_e6 / MICRO

    @property
    def offset(self) -> float:
        return self.offset_e6 / MICRO

    @property
    def grid(self) -> Grid:
        return Grid(*(v / NANO for v in self.grid_e9), self.nx, self.ny)

    def cell_offset(self, lon, lat) -> np.ndarray:
        """The cell offsets at (lon, lat): bilinear between cell centres, clamped at the edge cells
        (balance.offset_at)."""
        return offset_at(self.grid, self._cells, lon, lat)[0]

    def apply_global(self, n) -> np.ndarray:
        """Gain and offset only (no cells), clamped to [-1, 1]; NaN stays NaN. For reports."""
        return np.clip(self.gain * np.asarray(n, np.float64) + self.offset, -1.0, 1.0)

    def apply(self, n, lon, lat) -> np.ndarray:
        """Reference NDVI at (lon, lat) on the target's scale, clamped to [-1, 1]; NaN stays NaN."""
        v = self.gain * np.asarray(n, np.float64) + self.offset + self.cell_offset(lon, lat)
        return np.clip(v, -1.0, 1.0)

    def to_json(self) -> bytes:
        w, s, cl, ca = self.grid_e9
        d = {
            "format": FORMAT,
            "reference": self.reference,
            "target": self.target,
            "gain_e6": self.gain_e6,
            "offset_e6": self.offset_e6,
            "grid": {"west_e9": w, "south_e9": s, "cell_lon_e9": cl, "cell_lat_e9": ca, "nx": self.nx, "ny": self.ny},
            "cell_offsets_e6": [list(r) for r in self.cell_offsets_e6],
        }
        return canonical_json(d).encode()

    @classmethod
    def from_json(cls, data: bytes) -> NdviFit:
        d = json.loads(data)
        if d.get("format") != FORMAT:
            raise ValueError(f"ndvi fit format {d.get('format')!r} != {FORMAT}: rebuild the package to refit it")
        g = d["grid"]
        e9 = (int(g["west_e9"]), int(g["south_e9"]), int(g["cell_lon_e9"]), int(g["cell_lat_e9"]))
        rows = tuple(tuple(int(v) for v in r) for r in d["cell_offsets_e6"])
        return cls(
            d["reference"], d["target"], int(d["gain_e6"]), int(d["offset_e6"]), e9, int(g["nx"]), int(g["ny"]), rows
        )


def linear_fit(x: np.ndarray, y: np.ndarray) -> tuple[float, float]:
    """Least-squares (gain, offset) for y ~ gain * x + offset."""
    gain, offset = np.polyfit(np.asarray(x, np.float64), np.asarray(y, np.float64), 1)
    return float(gain), float(offset)


def fit_samples(reference: str, target: str, x, y, lon, lat, grid: Grid, s: dict) -> NdviFit:
    """The fit from fit samples alone: gain and offset by least squares, then each cell's median residual
    (target - fitted reference) where the cell has s["min_cell_samples"], median-filtered and filled outward with
    decay as the colour match's offsets (balance.cell_medians, balance.fill_offsets). report: cells, cells_filled."""
    x, y = np.asarray(x, np.float64), np.asarray(y, np.float64)
    g = NdviFit.make(reference, target, *linear_fit(x, y), grid)
    ix, iy = grid.index(lon, lat)
    ncells = grid.nx * grid.ny
    resid = y - (g.gain * x + g.offset)  # quantised gain and offset: the same residuals for any sample order
    values = cell_medians(resid[None], iy * grid.nx + ix, ncells, s["min_cell_samples"]).reshape(1, grid.ny, grid.nx)
    cells = fill_offsets(values, s["cell_km"], s["decay_km"], median_size=s["median_filter"])[0]
    f = NdviFit.make(reference, target, g.gain, g.offset, grid, cells)
    fitted = int(np.isfinite(values[0]).sum())
    f.report = {"cells": fitted, "cells_filled": ncells - fitted}
    return f


def _r4(v) -> float | None:
    return None if v is None or not np.isfinite(v) else round(float(v), 4)


def fit_ndvi(index, classes, bounds: Bounds, s: dict, dates: dict | None = None) -> NdviFit | None:
    """Fit on lattice nodes over `bounds` (the reference's footprints) where the reference and the target are both
    valid and the land cover isn't excluded: even (i + j) nodes fit, odd ones are held out for the report, which gives
    each reference acquisition date's residual bias (`dates`: reference qid -> date). The cell grid covers `bounds`
    at s["cell_km"]. None with fewer than s["min_samples"] fit samples."""
    from .layers.ndvi import sample_ndvi

    dates = dates or {}
    parts = []
    for b in shared_land_samples(index, classes, bounds, s, sample_ndvi):
        k = b.keep
        day = np.array([dates.get(e.qid, "") for e in b.ref_entries], dtype=object)[b.ref_which[k]]
        parts.append((b.ref_values[k], b.target_values[k], b.held[k], day, b.lon[k], b.lat[k]))
    if not parts:
        log.warning("ndvi fit: no shared land samples between %s and %s", s["reference"], s["target"])
        return None
    x, y, held, day, lon, lat = (np.concatenate([p[k] for p in parts]) for k in range(6))
    fit = ~held
    if fit.sum() < s["min_samples"]:
        log.warning("ndvi fit: only %d shared land samples; not fitted", int(fit.sum()))
        return None
    grid = Grid.covering(bounds, s["cell_km"], (bounds[1] + bounds[3]) / 2)
    f = fit_samples(s["reference"], s["target"], x[fit], y[fit], lon[fit], lat[fit], grid, s)
    xh, yh = x[held], y[held]
    before, glob, after = yh - xh, yh - f.apply_global(xh), yh - f.apply(xh, lon[held], lat[held])
    by_date = {}
    for d in sorted(set(day[held].tolist())):
        m = day[held] == d
        by_date[d or "unknown"] = {"n": int(m.sum()), "bias": _r4(np.median(after[m]))}
    has = before.size > 0
    cells = np.asarray(f.cell_offsets_e6) / MICRO
    f.report = {
        "samples_fit": int(fit.sum()),
        "samples_heldout": int(held.sum()),
        "gain": f.gain,
        "offset": f.offset,
        "grid": {"nx": f.nx, "ny": f.ny, "cell_km": s["cell_km"]},
        **f.report,
        "cell_offset_min": _r4(cells.min()),
        "cell_offset_max": _r4(cells.max()),
        "heldout": {
            "mae_before": _r4(np.abs(before).mean()) if has else None,
            "mae_global": _r4(np.abs(glob).mean()) if has else None,
            "mae_after": _r4(np.abs(after).mean()) if has else None,
            "bias_before": _r4(np.median(before)) if has else None,
            "bias_after": _r4(np.median(after)) if has else None,
        },
        "bias_after_by_date": by_date,
    }
    log.info(
        "ndvi fit: gain %.6f offset %.6f, %d cells (%d filled), held out %s",
        f.gain,
        f.offset,
        f.report["cells"],
        f.report["cells_filled"],
        json.dumps(f.report["heldout"]),
    )
    return f
