"""R1 chunk 2 NDVI gates (spec acceptance 3 and 4) and a false-colour overview for review.

    uv run --project scripts/scene python scripts/scene/tools/ndvi_check.py PKG --cache .cache/scene \
        [--overview OUT.png] [--step-m 100]

Values come from the package's NDVI tiles, the deepest tile holding each point (nearest pixel).
Gate 3 (plausibility): median NDVI per WorldCover class on a lattice over the bbox: tree cover (10) above
shrubland (20) and grassland (30), both above built-up (50) and bare (60), permanent water (80) below 0. Classes with
fewer than MIN_N samples are reported and left out of the comparisons.
Gate 4 (seam): land points every --step-m along NAIP's footprint edge, INSIDE_M inside (pure NAIP) and OUTSIDE_M
outside (pure Sentinel-2): |median(inside - outside)| <= SEAM_MAX. The same median with NAIP NDVI
inside unfitted (bias_unfitted) and with the global fit only, no cell offsets (bias_global), is reported beside it. Pairs whose inside point is less than 0.95 INSIDE_M from the edge (near corners) are dropped.
The manifest's NAIP footprints are dilated by about one coarse overview pixel (~80 m), so a point INSIDE_M inside
one can still be in the build's 200 m feather: only pairs whose inside point is pure NAIP (reference weight 1 at the
deepest NDVI zoom, as the build computes it, water buffer included) are kept ("candidates" counts them before).
Prints a JSON report; exits 1 when a gate fails, 2 when the package has no NDVI layer."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import shapely
from PIL import Image

from camsim_scene.cache import Cache
from camsim_scene.config import TILE_PX
from camsim_scene.context import WorkerState, class_rasters, reference_footprints
from camsim_scene.layers import imagery, ndvi
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import make_context
from camsim_scene.sources.base import M_PER_DEG
from camsim_scene.tiling import tile_size_deg
from camsim_scene.water import class_codes

MIN_N = 50
INSIDE_M, OUTSIDE_M = 250.0, 50.0
INSIDE_MIN = 0.95  # of INSIDE_M: an inside point nearer the edge (a corner) drops its pair
SEAM_MAX = 0.03
TREE, SHRUB, GRASS, BUILT, BARE, WATER = 10, 20, 30, 50, 60, 80
NODATA_RGB = (40, 60, 120)
OVERVIEW_PX = 2048


def _tile_pixel(z: int, lon: np.ndarray, lat: np.ndarray):
    """(tile x, tile y, row, col) of the pixel holding each point at zoom z."""
    t = tile_size_deg(z)
    fx, fy = (lon + 180.0) / t, (lat + 90.0) / t
    x, y = np.floor(fx).astype(np.int64), np.floor(fy).astype(np.int64)
    col = np.clip(((fx - x) * TILE_PX).astype(np.int64), 0, TILE_PX - 1)
    row = np.clip(((1.0 - (fy - y)) * TILE_PX).astype(np.int64), 0, TILE_PX - 1)
    return x, y, row, col


class NdviTiles:
    """NDVI at points from the deepest tile holding each one (nearest pixel; NaN where no tile)."""

    def __init__(self, pkg: Path):
        root = pkg / "ndvi"
        self.root = root
        self.zooms = sorted((int(p.name) for p in root.iterdir() if p.is_dir() and p.name.isdigit()), reverse=True)
        self._codes: dict = {}

    def _tile(self, z: int, x: int, y: int):
        key = (z, x, y)
        if key not in self._codes:
            p = self.root / str(z) / str(x) / f"{y}.png"
            self._codes[key] = ndvi.read_png(p.read_bytes()) if p.exists() else None
        return self._codes[key]

    def at(self, lon, lat) -> np.ndarray:
        lon, lat = np.asarray(lon, np.float64), np.asarray(lat, np.float64)
        out = np.full(lon.shape, np.nan)
        todo = np.ones(lon.shape, bool)
        for z in self.zooms:
            if not todo.any():
                break
            x, y, row, col = _tile_pixel(z, lon, lat)
            for i in np.flatnonzero(todo):
                code = self._tile(z, int(x.flat[i]), int(y.flat[i]))
                if code is not None:
                    out.flat[i] = ndvi.decode(code[row.flat[i], col.flat[i]])
                    todo.flat[i] = False
        return out


def class_order_failures(med: dict[int, float]) -> list[str]:
    tree = [med[c] for c in (TREE,) if c in med]
    mid = [med[c] for c in (SHRUB, GRASS) if c in med]
    low = [med[c] for c in (BUILT, BARE) if c in med]
    if not (tree and mid and low):
        return ["too few classes sampled (need tree cover, shrub or grass, built-up or bare)"]
    fails = []
    if not tree[0] > max(mid):
        fails.append(f"tree cover {tree[0]:.3f} not above shrub/grass {max(mid):.3f}")
    if not min(mid) > max(low):
        fails.append(f"shrub/grass {min(mid):.3f} not above built-up/bare {max(low):.3f}")
    if WATER in med and not med[WATER] < 0:
        fails.append(f"permanent water {med[WATER]:.3f} not below 0")
    return fails


def seam_pairs(footprints, step_m: float):
    """Points every step_m along the exterior of the footprints' union, moved INSIDE_M inward and OUTSIDE_M outward
    along the local normal (degrees of latitude on both axes, as the build); pairs whose inside point isn't inside,
    is nearer the edge than INSIDE_MIN x INSIDE_M (corners) or whose outside point isn't outside (concave edges) are
    dropped. (inside (2, n), outside (2, n))."""
    union = shapely.union_all(list(footprints))
    step = step_m / M_PER_DEG
    ins, outs = [], []
    for poly in getattr(union, "geoms", [union]):
        ring = poly.exterior
        d = np.arange(int(ring.length / step)) * step
        p = shapely.get_coordinates(shapely.line_interpolate_point(ring, d))
        q = shapely.get_coordinates(shapely.line_interpolate_point(ring, d + 0.1 * step))
        t = q - p
        t /= np.linalg.norm(t, axis=1, keepdims=True)
        nrm = np.column_stack([-t[:, 1], t[:, 0]])
        probe = p + nrm * (INSIDE_M / M_PER_DEG)
        sign = np.where(shapely.contains_xy(union, probe[:, 0], probe[:, 1]), 1.0, -1.0)[:, None]
        ins.append(p + sign * nrm * (INSIDE_M / M_PER_DEG))
        outs.append(p - sign * nrm * (OUTSIDE_M / M_PER_DEG))
    if not ins:
        return np.empty((2, 0)), np.empty((2, 0))
    a, b = np.concatenate(ins), np.concatenate(outs)
    edge = shapely.boundary(union)
    depth = shapely.distance(edge, shapely.points(a)) * M_PER_DEG
    keep = shapely.contains_xy(union, a[:, 0], a[:, 1]) & ~shapely.contains_xy(union, b[:, 0], b[:, 1])
    keep &= depth >= INSIDE_MIN * INSIDE_M
    return a[keep].T, b[keep].T


def pure_reference(st: WorkerState, s: dict, z: int, lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
    """Where the reference (NAIP) is valid with weight 1 at zoom z's pixel holding each point: the build's feather
    and water buffer (as verify's ndvi_values), so nothing of Sentinel-2 is blended in."""
    out = np.zeros(lon.shape, bool)
    x, y, row, col = _tile_pixel(z, lon, lat)
    buffer_m = st.ndvi_water.buffer_m if st.ndvi_water is not None else 0.0
    for tx, ty in sorted(set(zip(x.tolist(), y.tolist(), strict=True))):
        i = np.flatnonzero((x == tx) & (y == ty))
        entries = st.index["ndvi"].query(imagery.query_bounds(z, tx, ty, s["feather_m"]))
        ref = [e for e in entries if imagery.source_of(e) == s["reference"]]
        if not ref:
            continue
        allow, dpx = ndvi.water_allow(z, tx, ty, st.ndvi_water, s["feather_m"])
        wt = ndvi.reference_weight(z, tx, ty, ref, s["feather_m"], allow, dpx, buffer_m)[row[i], col[i]]
        (glon, glat), d = imagery.pixel_grid(z, tx, ty)
        _, rok, _ = ndvi.sample_ndvi(ref, glon[row[i], col[i]], glat[row[i], col[i]], d * M_PER_DEG)
        out[i] = rok & (wt >= 1.0)
    return out


def colourise(v: np.ndarray) -> np.ndarray:
    """NDVI -> brown (-0.2) / straw (0.2) / green (0.8); nodata NODATA_RGB."""
    stops = [-0.2, 0.2, 0.8]
    cols = np.array([[140, 100, 60], [220, 200, 120], [30, 140, 40]], np.float64)
    x = np.nan_to_num(v, nan=0.0)
    rgb = np.stack([np.interp(x, stops, cols[:, k]) for k in range(3)], axis=-1)
    rgb[np.isnan(v)] = NODATA_RGB
    return np.rint(rgb).astype(np.uint8)


def write_overview(tiles: NdviTiles, bbox, out: Path) -> None:
    w, s, e, n = bbox
    step = max(e - w, n - s) / OVERVIEW_PX
    lon, lat = np.meshgrid(np.arange(w + step / 2, e, step), np.arange(n - step / 2, s, -step))
    Image.fromarray(colourise(tiles.at(lon, lat))).save(out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--cache", type=Path, required=True)
    ap.add_argument("--overview", type=Path)
    ap.add_argument("--step-m", type=float, default=100.0)
    a = ap.parse_args(argv)
    m = Manifest.load(a.pkg / "manifest.json")
    s = m.layers.get("ndvi")
    if not s or not (a.pkg / "ndvi").is_dir():
        print(json.dumps({"error": "the package has no NDVI layer"}))
        return 2
    ctx = make_context(a.pkg, m, Cache(a.cache))
    st = WorkerState(ctx)
    classes, _ = class_rasters(m, ctx.asset_paths)
    tiles = NdviTiles(a.pkg)
    report: dict = {"zooms": tiles.zooms[::-1], "fails": []}
    step = a.step_m / M_PER_DEG
    w, s_, e, n = m.bbox
    lon, lat = np.meshgrid(np.arange(w + step / 2, e, step), np.arange(s_ + step / 2, n, step))
    v = tiles.at(lon, lat)
    codes = class_codes(classes, lon, lat) if classes else np.zeros(lon.shape, np.uint8)
    med, counts = {}, {}
    for c in np.unique(codes).tolist():
        k = (codes == c) & np.isfinite(v)
        counts[c] = int(k.sum())
        if k.sum() >= MIN_N:
            med[c] = round(float(np.median(v[k])), 4)
    report["class_median"], report["class_n"] = med, counts
    report["fails"] += class_order_failures(med)
    if s["reference"]:
        fps = reference_footprints(m, s["reference"])
        ins, outs = seam_pairs(fps, a.step_m)
        if not ins.shape[1]:
            report["seam"] = {"candidates": 0, "pairs": 0, "bias": None, "bias_global": None, "bias_unfitted": None}
            report["fails"].append("no seam pairs sampled")
            ins = outs = None
    if s["reference"] and ins is not None:
        keep = np.ones(ins.shape[1], bool)
        if classes:
            keep &= (class_codes(classes, *ins) != WATER) & (class_codes(classes, *outs) != WATER)
        vi, vo = tiles.at(*ins), tiles.at(*outs)
        keep &= np.isfinite(vi) & np.isfinite(vo)
        candidates = int(keep.sum())
        keep &= pure_reference(st, s, tiles.zooms[0], ins[0], ins[1])
        ref = [
            x for x in st.index["ndvi"].query(shapely.union_all(fps).bounds) if x.qid.startswith(s["reference"] + "/")
        ]
        tm = tile_size_deg(tiles.zooms[0]) / TILE_PX * M_PER_DEG
        raw, rok, _ = ndvi.sample_ndvi(ref, ins[0], ins[1], tm)
        kr = keep & rok
        bias = round(float(np.median(vi[keep] - vo[keep])), 4) if keep.any() else None
        fit = st.ndvi_fit
        glob = fit.apply_global(raw[kr]) - vo[kr] if fit is not None and kr.any() else None
        report["seam"] = {
            "candidates": candidates,
            "pairs": int(keep.sum()),
            "bias": bias,
            "bias_global": round(float(np.median(glob)), 4) if glob is not None else None,
            "bias_unfitted": round(float(np.median(raw[kr] - vo[kr])), 4) if kr.any() else None,
        }
        if bias is None:
            report["fails"].append("no seam pairs sampled")
        elif abs(bias) > SEAM_MAX:
            report["fails"].append(f"seam bias {bias:.4f} beyond {SEAM_MAX}")
    if a.overview:
        write_overview(tiles, m.bbox, a.overview)
    print(json.dumps(report, indent=1))
    return 1 if report["fails"] else 0


if __name__ == "__main__":
    sys.exit(main())
