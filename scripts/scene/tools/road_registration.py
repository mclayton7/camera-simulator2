"""REALISM R2 gate 2: OSM road centrelines vs NAIP roads, as one 2-D shift per window.

    uv run --project scripts/scene python scripts/scene/tools/road_registration.py PKG [--windows 60] [--seed 0] \
        [--json out.json] [--overlays DIR] [--cache .cache/scene]

The package bbox (not the ring) is tiled into WIN_M x WIN_M windows. A window qualifies when it holds at least
MIN_ROAD_M of motorway..residential centreline (links included) that burns as pavement (code 200 or 201), bridges and
tunnels excluded; up to --windows of them are picked with Random(seed). In each window, on a RES_M local metric grid:
  G = |gradient| (DN/m) of the package imagery luma (z17 TMS, bilinear), smoothed with a SMOOTH_SIGMA_M Gaussian
      first, over the window plus SEARCH_M on every side;
  B = the boundary of the OSM roads buffered to width_m / 2 (both edges of every road), rasterised BOUNDARY_W_M wide;
  I = the road surface inset INTERIOR_INSET_M from that boundary.
The zero-sum template B / |B| - I / |I| is correlated against G over integer shifts within +-SEARCH_M: each value is
the mean G on the shifted road edges minus the mean G on the shifted road interior (DN/m). The interior term matters:
house fronts and roof edges are stronger than kerbs and run parallel to the street, and B alone locks onto them; a
road's surface is smooth, a yard or a roof is not. The peak (dE, dN) is where the imagery's roads sit relative to OSM.
A window is used when peak >= MIN_CONTRAST_DN_PER_M (a featureless window has no edge-vs-interior contrast) and, with
m = the median of the correlation surface and q95 = its 95th percentile outside PEAK_EXCL_M of the peak,
(peak - m) >= PROMINENCE x (q95 - m) (an ambiguous, repetitive window); otherwise it is excluded and counted. The
ratio alone can't reject noise (pure noise scores 1.6-2.1), hence the floor. Windows with more than MAX_NAN_FRAC of
their imagery missing are excluded too.
Gate: median |shift| (Euclidean) <= GATE_M. The report also gives p90 and the signed mean (dE, dN), where a systematic
OSM offset would show. `--overlays DIR` writes G with B at zero shift (red) and at the found shift (green) for the
windows with the largest and the smallest shifts. Exits 1 when the gate fails."""

from __future__ import annotations

import argparse
import json
import math
import random
import sys
from pathlib import Path

import numpy as np
import shapely
from PIL import Image

GATE_M = 10.0
WIN_M = 1000.0
RES_M = 1.0
SEARCH_M = 20.0
SMOOTH_SIGMA_M = 1.0
BOUNDARY_W_M = 1.5  # B is rasterised this wide: 1-2 px at RES_M
MIN_ROAD_M = 1000.0
MAX_WINDOWS = 60
INTERIOR_INSET_M = 1.5  # the road interior I is the road surface this far inside its edges
MIN_CONTRAST_DN_PER_M = 1.0  # peak: mean G on the edges minus mean G on the interior
PROMINENCE = 1.25
PEAK_EXCL_M = 5.0
MAX_NAN_FRAC = 0.01
ZOOM = 17
CLASSES = ("motorway", "trunk", "primary", "secondary", "tertiary", "unclassified", "residential")
M_PER_DEG_LAT = 111_320.0


class TmsSampler:
    """Bilinear luma (Pillow 'L') from a geodetic TMS (origin -180/-90, y from the south, 256 px tiles); NaN where a
    tile is missing. Each call mosaics the tiles its points need once, then indexes the mosaic."""

    def __init__(self, root: Path, z: int):
        self.root, self.z = Path(root), z
        self.t = 180.0 / (1 << z)
        self._tiles: dict[tuple[int, int], np.ndarray | None] = {}

    def _tile(self, x: int, y: int) -> np.ndarray | None:
        k = (x, y)
        if k not in self._tiles:
            p = self.root / str(self.z) / str(x) / f"{y}.jpg"
            self._tiles[k] = np.asarray(Image.open(p).convert("L"), np.float64) if p.is_file() else None
        return self._tiles[k]

    def luma(self, lon, lat) -> np.ndarray:
        fx = (np.asarray(lon, np.float64) + 180.0) / self.t * 256 - 0.5
        fy = (np.asarray(lat, np.float64) + 90.0) / self.t * 256 - 0.5
        x0, y0 = np.floor(fx).astype(np.int64), np.floor(fy).astype(np.int64)
        wx, wy = fx - x0, fy - y0
        tx0, tx1 = int(x0.min()) // 256, int(x0.max() + 1) // 256
        ty0, ty1 = int(y0.min()) // 256, int(y0.max() + 1) // 256
        mos = np.full(((ty1 - ty0 + 1) * 256, (tx1 - tx0 + 1) * 256), np.nan)  # row = global row from the south
        for ty in range(ty0, ty1 + 1):
            for tx in range(tx0, tx1 + 1):
                a = self._tile(tx, ty)
                if a is not None:
                    r, c = (ty - ty0) * 256, (tx - tx0) * 256
                    mos[r : r + 256, c : c + 256] = a[::-1]
        cx, cy = x0 - tx0 * 256, y0 - ty0 * 256
        return (
            mos[cy, cx] * (1 - wx) * (1 - wy)
            + mos[cy, cx + 1] * wx * (1 - wy)
            + mos[cy + 1, cx] * (1 - wx) * wy
            + mos[cy + 1, cx + 1] * wx * wy
        )


class Local:
    """Equirectangular metres about (lon0, lat0)."""

    def __init__(self, lon0: float, lat0: float):
        self.lon0, self.lat0 = lon0, lat0
        self.kx = M_PER_DEG_LAT * math.cos(math.radians(lat0))

    def fwd(self, g):
        return shapely.transform(
            g, lambda c: np.column_stack(((c[:, 0] - self.lon0) * self.kx, (c[:, 1] - self.lat0) * M_PER_DEG_LAT))
        )

    def inv(self, x, y):
        return self.lon0 + np.asarray(x) / self.kx, self.lat0 + np.asarray(y) / M_PER_DEG_LAT


def road_ways(feats) -> list:
    """Motorway..residential ways (links included) that burn as pavement (200/201), bridges and tunnels excluded
    (code_for returns None for them)."""
    from camsim_scene.features import code_for

    return [
        f
        for f in feats
        if f.kind == "road"
        and f.geom.geom_type == "LineString"
        and f.tags.get("highway", "").removesuffix("_link") in CLASSES
        and code_for(f) in (200, 201)
    ]


def qualifying_windows(roads, bbox, seed: int, max_n: int) -> list[dict]:
    """Up to max_n WIN_M windows tiling bbox (W S E N) that hold >= MIN_ROAD_M of road centreline, picked with
    Random(seed) from the qualifying ones in grid order. Each: {i, j, lon, lat (centre), road_m}."""
    w, s, e, n = bbox
    loc = Local((w + e) / 2, (s + n) / 2)
    lines = [loc.fwd(f.geom) for f in roads]
    tree = shapely.STRtree(lines) if lines else None
    x0, y0 = loc.fwd(shapely.Point(w, s)).coords[0]
    x1, y1 = loc.fwd(shapely.Point(e, n)).coords[0]
    out = []
    for i in range(int((y1 - y0) // WIN_M)):
        for j in range(int((x1 - x0) // WIN_M)):
            cell = shapely.box(x0 + j * WIN_M, y0 + i * WIN_M, x0 + (j + 1) * WIN_M, y0 + (i + 1) * WIN_M)
            hits = tree.query(cell, predicate="intersects") if tree is not None else []
            road_m = float(sum(lines[k].intersection(cell).length for k in hits))
            if road_m >= MIN_ROAD_M:
                c = cell.centroid
                lon, lat = loc.inv(c.x, c.y)
                out.append({"i": i, "j": j, "lon": float(lon), "lat": float(lat), "road_m": round(road_m, 1)})
    if len(out) > max_n:
        keep = sorted(random.Random(seed).sample(range(len(out)), max_n))
        out = [out[k] for k in keep]
    return out


def road_masks(roads_local, bounds, res: float) -> tuple[np.ndarray, np.ndarray]:
    """(B, I): uint8 rasters (row 0 = north) over bounds (x0, y0, x1, y1, metres). B = the boundary of the union of the
    roads buffered to width / 2 (flat caps, round joins), BOUNDARY_W_M wide; I = the road surface inset by
    INTERIOR_INSET_M from that boundary (empty for roads narrower than twice the inset). roads_local: [(line, width)]."""
    from rasterio import features as rf
    from rasterio.transform import Affine

    x0, y0, x1, y1 = bounds
    shape = (int(round((y1 - y0) / res)), int(round((x1 - x0) / res)))
    win = shapely.box(x0, y0, x1, y1)
    tf = Affine(res, 0, x0, 0, -res, y1)
    out = []
    surface = shapely.union_all([g.buffer(w / 2.0, cap_style="flat", join_style="round") for g, w in roads_local])
    for geom in (surface.boundary.buffer(BOUNDARY_W_M / 2.0), surface.buffer(-INTERIOR_INSET_M)):
        geom = geom.intersection(win)
        empty = geom.is_empty or geom.area == 0
        out.append(np.zeros(shape, np.uint8) if empty else rf.rasterize([(geom, 1)], out_shape=shape, transform=tf))
    return out[0], out[1]


def gradient_magnitude(luma: np.ndarray, res: float) -> np.ndarray:
    from scipy.ndimage import gaussian_filter

    gy, gx = np.gradient(gaussian_filter(luma, SMOOTH_SIGMA_M / res, mode="nearest"), res)
    return np.hypot(gx, gy)


def window_shift(g_ext: np.ndarray, b: np.ndarray, interior: np.ndarray, res: float, search_m: float) -> dict:
    """Correlate the zero-sum template T = B / |B| - I / |I| (the window) against G (the window plus search_m on every
    side; both row 0 = north) over every integer shift: c = mean G on the shifted road edges - mean G on the shifted
    road interior, in DN/m. Returns {dE, dN (m: where the imagery's roads sit relative to OSM), peak, median, q95,
    used}; `used` is the prominence rule (module docstring)."""
    from scipy.signal import correlate

    m = int(round(search_m / res))
    nb, ni = int(b.sum()), int(interior.sum())
    if nb == 0:
        return {"dE": 0.0, "dN": 0.0, "peak": 0.0, "median": 0.0, "q95": 0.0, "used": False}
    t = b / nb - (interior / ni if ni else 0.0)
    c = correlate(np.asarray(g_ext, np.float64), t, mode="valid", method="fft")  # c[i, j] = sum t * g[i:, j:]
    i, j = np.unravel_index(int(np.argmax(c)), c.shape)
    yy, xx = np.indices(c.shape)
    outside = np.hypot(yy - i, xx - j) * res > PEAK_EXCL_M
    peak, med = float(c[i, j]), float(np.median(c))
    q95 = float(np.percentile(c[outside], 95)) if outside.any() else peak
    used = bool(peak >= MIN_CONTRAST_DN_PER_M and peak - med >= PROMINENCE * (q95 - med))
    return {"dE": (j - m) * res, "dN": -(i - m) * res, "peak": peak, "median": med, "q95": q95, "used": used}


def measure_window(sampler: TmsSampler, roads, win: dict) -> dict:
    """G and B for one window (a local frame about its centre) and their shift."""
    from camsim_scene.features import width_m

    loc = Local(win["lon"], win["lat"])
    h, ext = WIN_M / 2, WIN_M / 2 + SEARCH_M
    near = shapely.box(*loc.inv(-ext, -ext), *loc.inv(ext, ext))
    rl = [(loc.fwd(f.geom), width_m(f)) for f in roads if f.geom.intersects(near)]
    b, interior = road_masks(rl, (-h, -h, h, h), RES_M)
    n = int(round(2 * ext / RES_M))
    xs = -ext + (np.arange(n) + 0.5) * RES_M
    ys = ext - (np.arange(n) + 0.5) * RES_M
    xx, yy = np.meshgrid(xs, ys)
    lon, lat = loc.inv(xx.ravel(), yy.ravel())
    luma = sampler.luma(lon, lat).reshape(n, n)
    nan = float(np.isnan(luma).mean())
    out = {**win, "nan_frac": round(nan, 4)}
    if nan > MAX_NAN_FRAC:
        return {**out, "used": False, "reason": "imagery missing"}
    luma = np.where(np.isnan(luma), np.nanmean(luma), luma)
    g = gradient_magnitude(luma, RES_M)
    r = window_shift(g, b, interior, RES_M, SEARCH_M)
    out.update({k: (round(v, 3) if isinstance(v, float) else v) for k, v in r.items()})
    out["shift_m"] = round(math.hypot(r["dE"], r["dN"]), 3)
    if not r["used"]:
        out["reason"] = "not prominent"
    out["_g"], out["_b"] = g, b
    return out


def save_overlay(w: dict, path: Path) -> None:
    """G (grey, clipped at its 99th percentile) with B at zero shift in red and at the found shift in green."""
    g, b = w["_g"], w["_b"]
    m = int(round(SEARCH_M / RES_M))
    v = np.clip(g / max(np.percentile(g, 99), 1e-9) * 255, 0, 255).astype(np.uint8)
    rgb = np.repeat(v[..., None], 3, axis=2)
    hh, ww = b.shape
    rgb[m : m + hh, m : m + ww][b > 0] = (255, 0, 0)
    di, dj = -int(round(w["dN"] / RES_M)), int(round(w["dE"] / RES_M))
    rgb[m + di : m + di + hh, m + dj : m + dj + ww][b > 0] = (0, 255, 0)
    Image.fromarray(rgb).save(path)


def run(pkg: Path, cache_root: Path | None, n_windows: int, seed: int, overlays: Path | None) -> dict:
    from camsim_scene.cache import Cache, default_cache_root
    from camsim_scene.manifest import Manifest
    from camsim_scene.pipeline import _load_features, make_context

    m = Manifest.load(pkg / "manifest.json")
    fs = m.layers["landcover"].get("features")
    if not fs or "osm_geofabrik" not in fs["sources"]:
        raise SystemExit(f"{pkg}: the manifest has no OSM feature source")
    feats = _load_features(m, make_context(pkg, m, Cache(cache_root or default_cache_root())))
    roads = road_ways(feats)
    wins = qualifying_windows(roads, tuple(m.bbox), seed, n_windows)
    sampler = TmsSampler(pkg / "imagery", ZOOM)
    rows = []
    for win in wins:
        rows.append(measure_window(sampler, roads, win))
        sampler._tiles.clear()  # windows don't share many tiles; keep memory flat
    used = [r for r in rows if r["used"]]
    sh = np.array([r["shift_m"] for r in used])
    med = float(np.median(sh)) if sh.size else None
    report = {
        "pkg": str(pkg),
        "seed": seed,
        "method": "2-D windowed correlation of OSM road boundaries vs NAIP gradient magnitude",
        "constants": {
            "WIN_M": WIN_M,
            "RES_M": RES_M,
            "SEARCH_M": SEARCH_M,
            "SMOOTH_SIGMA_M": SMOOTH_SIGMA_M,
            "BOUNDARY_W_M": BOUNDARY_W_M,
            "MIN_ROAD_M": MIN_ROAD_M,
            "INTERIOR_INSET_M": INTERIOR_INSET_M,
            "MIN_CONTRAST_DN_PER_M": MIN_CONTRAST_DN_PER_M,
            "PROMINENCE": PROMINENCE,
            "PEAK_EXCL_M": PEAK_EXCL_M,
        },
        "n_windows": len(rows),
        "n_used": len(used),
        "n_excluded": len(rows) - len(used),
        "median_shift_m": med,
        "p90_shift_m": float(np.percentile(sh, 90)) if sh.size else None,
        "mean_dE_m": float(np.mean([r["dE"] for r in used])) if used else None,
        "mean_dN_m": float(np.mean([r["dN"] for r in used])) if used else None,
        "gate_m": GATE_M,
        "pass": med is not None and med <= GATE_M,
        "roads": len(roads),
        "windows": [{k: v for k, v in r.items() if not k.startswith("_")} for r in rows],
    }
    if overlays:
        overlays.mkdir(parents=True, exist_ok=True)
        ranked = sorted(used, key=lambda r: -r["shift_m"])
        for r in ranked[:4] + ranked[-2:]:
            save_overlay(r, overlays / f"win_{r['i']:02d}_{r['j']:02d}_{r['shift_m']:.1f}m.png")
    return report


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--windows", type=int, default=MAX_WINDOWS)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--cache", type=Path, help="fetch cache (default .cache/scene or $CAMSIM_SCENE_CACHE)")
    ap.add_argument("--overlays", type=Path, help="write per-window overlay PNGs here")
    a = ap.parse_args(argv)
    r = run(a.pkg, a.cache, a.windows, a.seed, a.overlays)

    def fmt(v):
        return "n/a" if v is None else f"{v:.2f} m"

    print(
        f"road registration: median |shift| {fmt(r['median_shift_m'])}, p90 {fmt(r['p90_shift_m'])}, "
        f"mean (dE, dN) ({fmt(r['mean_dE_m'])}, {fmt(r['mean_dN_m'])}), windows used {r['n_used']}, "
        f"excluded {r['n_excluded']} -> {'PASS' if r['pass'] else 'FAIL'} (gate {GATE_M:.0f} m)"
    )
    if a.json:
        a.json.parent.mkdir(parents=True, exist_ok=True)
        a.json.write_text(json.dumps(r, indent=1) + "\n")
    return 0 if r["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
