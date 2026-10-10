"""Sentinel-2 -> NAIP colour match (REALISM R1; docs/superpowers/specs/2026-10-09-naip-edge-design.md).

A per-band tone curve (quantile match, raw Sentinel-2 DN -> NAIP DN) plus a smooth per-band offset on a coarse grid,
fitted once per build (fit_balance) and stored as imagery/balance.json. Every stored value is quantised to an
integer step, so float summation order can't change the file; workers always read the model back from it."""

from __future__ import annotations

import json
import logging
import math
from dataclasses import dataclass, field

import numpy as np
from scipy.ndimage import distance_transform_edt

from .manifest import canonical_json
from .sources.base import M_PER_DEG
from .tiling import Bounds

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


def offset_at(grid: Grid, offsets: np.ndarray, lon, lat) -> np.ndarray:
    """offsets (bands, ny, nx) at (lon, lat): bilinear between cell centres, clamped at the grid's edge cells."""
    g = grid
    fx = np.clip((np.asarray(lon, np.float64) - g.west) / g.cell_lon - 0.5, 0.0, g.nx - 1.0)
    fy = np.clip((np.asarray(lat, np.float64) - g.south) / g.cell_lat - 0.5, 0.0, g.ny - 1.0)
    x0 = np.minimum(np.floor(fx).astype(np.int64), max(g.nx - 2, 0))
    y0 = np.minimum(np.floor(fy).astype(np.int64), max(g.ny - 2, 0))
    x1, y1 = np.minimum(x0 + 1, g.nx - 1), np.minimum(y0 + 1, g.ny - 1)
    tx, ty = fx - x0, fy - y0
    o = offsets
    return (
        o[:, y0, x0] * (1 - tx) * (1 - ty)
        + o[:, y0, x1] * tx * (1 - ty)
        + o[:, y1, x0] * (1 - tx) * ty
        + o[:, y1, x1] * tx * ty
    )


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
        return offset_at(self.grid, self.offsets, lon, lat)

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


log = logging.getLogger(__name__)
BLOCK = 512  # lattice nodes per block side (~5 km at 10 m)


def lattice_blocks(bounds, step_deg: float, block: int = BLOCK):
    """Nodes at integer multiples of step_deg covering bounds, in blocks north to south, west to east:
    (lon, lat, i, j) with i, j the global node indices (held-out split: (i + j) odd)."""
    w, s, e, n = bounds
    i0, i1 = math.floor(w / step_deg), math.ceil(e / step_deg)
    j0, j1 = math.floor(s / step_deg), math.ceil(n / step_deg)
    for jb in range(j1, j0, -block):
        jj = np.arange(jb, max(j0, jb - block), -1)
        for ib in range(i0, i1, block):
            ii = np.arange(ib, min(i1, ib + block))
            i, j = np.meshgrid(ii, jj)
            yield i * step_deg, j * step_deg, i, j


def land_mask(classes, lon, lat, target_m: float, exclude) -> np.ndarray:
    """True where the land-cover class is known and not excluded. Bilinear class values that aren't integral (class
    boundaries) are dropped. No class rasters: everything counts as land."""
    from .sources.base import project

    if not classes:
        return np.ones(lon.shape, bool)
    v = np.full(lon.shape, np.nan)
    ok = np.zeros(lon.shape, bool)
    for r in classes:
        if ok.all():
            break
        px, py = project(r, lon, lat)
        vals, good = r.sample(px, py, target_m)
        take = good & ~ok
        v[take] = vals[0][take]
        ok |= take
    code = np.rint(np.nan_to_num(v))
    return ok & (np.abs(v - code) < 1e-6) & ~np.isin(code, list(exclude))


@dataclass
class LatticeBlock:
    """One lattice block of shared land samples: the reference's and the target's values, where both are valid on
    land (`keep`), which nodes are held out, and the reference entries with the winning one's index per node."""

    lon: np.ndarray
    lat: np.ndarray
    held: np.ndarray
    keep: np.ndarray
    ref_entries: list
    ref_values: np.ndarray
    ref_which: np.ndarray
    target_values: np.ndarray


def shared_land_samples(index, classes, bounds, s: dict, sample):
    """Lattice blocks over `bounds` at s["fit_step_m"] where the reference and the target both have data, sampled by
    `sample(entries, lon, lat, target_m) -> (values, valid, winning entry index)`; blocks with no kept node are
    skipped. Even (i + j) nodes fit, odd ones (`held`) are held out for the report."""
    from .layers.imagery import source_of

    step_m = s["fit_step_m"]
    exclude = set(s["exclude_classes"])
    for lon, lat, i, j in lattice_blocks(bounds, step_m / M_PER_DEG):
        entries = index.query((float(lon.min()), float(lat.min()), float(lon.max()), float(lat.max())))
        ref = [e for e in entries if source_of(e) == s["reference"]]
        tgt = [e for e in entries if source_of(e) == s["target"]]
        if not ref or not tgt:
            continue
        rv, rok, rw = sample(ref, lon, lat, step_m)
        tv, tok, _ = sample(tgt, lon, lat, step_m)
        keep = rok & tok & land_mask(classes, lon, lat, step_m, exclude)
        if keep.any():
            yield LatticeBlock(lon, lat, (i + j) % 2 == 1, keep, ref, rv, rw, tv)


def grid_for(m, cell_km: float) -> Grid:
    ring = next(r for r in m.region_objs() if r.name == "ring")
    return Grid.covering(ring.geometry().bounds, cell_km, (m.bbox[1] + m.bbox[3]) / 2)


def _stats(pred, ref, cells, ncells, min_n) -> dict:
    e = pred.astype(np.float64) - ref
    cb = np.abs(cell_medians(e, cells, ncells, min_n))
    good = np.isfinite(cb[0])

    def r2(v):
        return [round(float(x), 2) for x in v]

    return {
        "mae_dn": r2(np.abs(e).mean(axis=1)),
        "bias_dn": r2(e.mean(axis=1)),
        "cells": int(good.sum()),
        "cell_bias_median_dn": r2(np.median(cb[:, good], axis=1)) if good.any() else None,
        "cell_bias_p90_dn": r2(np.percentile(cb[:, good], 90, axis=1)) if good.any() else None,
    }


def fit_balance(index, classes, bounds: Bounds, grid: Grid, s: dict) -> Balance | None:
    """Fit the colour match on a lattice over `bounds` (the reference's footprints): nodes where the reference and
    the target are both valid and the land cover isn't excluded. Even (i + j) nodes fit, odd ones are held out for
    the report. None when there are too few shared land samples."""
    from .layers.imagery import sample_entries
    from .sources.base import DECODERS

    parts = []
    for b in shared_land_samples(index, classes, bounds, s, sample_entries):
        keep, lon, lat = b.keep, b.lon, b.lat
        rv, tv = b.ref_values, b.target_values
        ix, iy = grid.index(lon[keep], lat[keep])
        parts.append(
            (
                np.clip(np.rint(rv[:, keep]), 0, 255).astype(np.uint8),
                np.clip(np.rint(tv[:, keep]), 0, 65535).astype(np.uint16),
                (iy * grid.nx + ix).astype(np.int32),
                b.held[keep],
                lon[keep].astype(np.float32),
                lat[keep].astype(np.float32),
            )
        )
    if not parts:
        log.warning("balance: no shared land samples between %s and %s", s["reference"], s["target"])
        return None
    ref, raw, cell, held, lon, lat = (np.concatenate([p[k] for p in parts], axis=-1) for k in range(6))
    fit = ~held
    if fit.sum() < s["min_cell_samples"]:
        log.warning("balance: only %d shared land samples; not fitted", int(fit.sum()))
        return None
    qs = np.linspace(0.0, 1.0, s["quantiles"])
    xq = [np.quantile(raw[b, fit], qs) for b in range(3)]
    yq = [np.quantile(ref[b, fit], qs) for b in range(3)]
    ncells = grid.nx * grid.ny
    tone_only = Balance.make(s["reference"], s["target"], s["feather_m"], xq, yq, grid, np.zeros(3 * ncells))
    resid = ref[:, fit] - tone_only.tone(raw[:, fit])
    values = cell_medians(resid, cell[fit], ncells, s["min_cell_samples"]).reshape(3, grid.ny, grid.nx)
    offsets = fill_offsets(values, s["cell_km"], s["decay_km"], median_size=s["median_filter"])
    bal = Balance.make(s["reference"], s["target"], s["feather_m"], xq, yq, grid, offsets)
    rh, wh, ch = ref[:, held].astype(np.float64), raw[:, held], cell[held]
    lonh, lath = lon[held].astype(np.float64), lat[held].astype(np.float64)
    min_h = max(1, s["min_cell_samples"] // 2)
    bal.report = {
        "samples_fit": int(fit.sum()),
        "samples_heldout": int(held.sum()),
        "cells_fitted": int(np.isfinite(values[0]).sum()),
        "heldout": {
            "before": _stats(DECODERS["s2_reflectance"](wh.astype(np.float64)), rh, ch, ncells, min_h),
            "after": _stats(bal.apply(wh, lonh, lath), rh, ch, ncells, min_h),
        },
    }
    log.info("balance: %s", json.dumps(bal.report["heldout"]))
    return bal
