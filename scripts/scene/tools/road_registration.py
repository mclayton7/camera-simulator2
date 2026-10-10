"""REALISM R2 gate 2: OSM road centrelines vs NAIP roads. Samples N points (seeded) on motorway..tertiary centrelines at
least 50 m from a junction or a skipped (bridge/tunnel) way, reads a +-30 m luminance profile across the road from the
package imagery (z17 TMS, bilinear), and finds the offset of the best match to a dark box of the road's width. Gate:
median |offset| <= 10 m. Profiles without a dark band (concrete, shade, trees) are excluded and counted.

    uv run --project scripts/scene python scripts/scene/tools/road_registration.py PKG [--n 200] [--seed 0] \
        [--json out.json] [--crops DIR] [--cache .cache/scene]

Only ways that burn as dark asphalt (code 200) are sampled: concrete is excluded by design, and `_link` ramps are left
out (they curve and sit next to junctions). A junction is a node shared by two or more road ways of any class. The
offset is signed along the left normal of the way's direction. `--crops DIR` writes a road-aligned luma crop (80 m
along x 60 m across) of the worst profiles, centreline in red, best match in green. Exits 1 when the gate fails."""

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

MIN_CONTRAST_DN = 15.0
GATE_M = 10.0
HALF_M = 30.0
STEP_M = 0.5
CLEAR_M = 50.0  # from a junction, a skipped way and the margin edge
ZOOM = 17
CLASSES = ("motorway", "trunk", "primary", "secondary", "tertiary")
M_PER_DEG_LAT = 111_320.0


def best_offset(profile, step_m, road_w_m):
    """Signed offset (m) from the profile centre of the best dark box of the road's width against its two flanks (out
    to three half-widths from the box centre), or None when no box is MIN_CONTRAST_DN darker than BOTH flanks. Scoring
    by the darker flank, not the whole ring, keeps the dark ground beside a bright (concrete) road from matching: its
    ring holds the bright road on one side only. Each flank needs at least half a half-width of samples."""
    n = len(profile)
    half = max(1, int(round(road_w_m / 2 / step_m)))
    min_flank = max(1, half // 2)
    best, best_c = None, MIN_CONTRAST_DN
    for c in range(half, n - half):
        lo, hi = max(0, c - 3 * half), min(n, c + 3 * half + 1)
        left, right = profile[lo : c - half], profile[c + half + 1 : hi]
        if left.size < min_flank or right.size < min_flank:
            continue
        contrast = min(left.mean(), right.mean()) - profile[c - half : c + half + 1].mean()
        if contrast > best_c:
            best, best_c = c, contrast
    return None if best is None else (best - (n - 1) / 2) * step_m


def split_ways(feats) -> tuple[list, list]:
    """(sampled ways, skipped road/rail ways): main roads that burn as dark asphalt, and bridges/tunnels to keep clear of."""
    from camsim_scene.features import _skipped, code_for

    kept, skipped = [], []
    for f in feats:
        if f.kind not in ("road", "rail") or f.geom.geom_type != "LineString":
            continue
        if _skipped(f.tags):
            skipped.append(f)
        elif f.kind == "road" and f.tags.get("highway") in CLASSES and code_for(f) == 200:
            kept.append(f)
    return kept, skipped


def junctions(feats) -> list[tuple[float, float]]:
    """Nodes (rounded to 1e-7 deg) shared by two or more road ways, sorted."""
    seen: dict[tuple[float, float], int] = {}
    for f in feats:
        if f.kind != "road" or f.geom.geom_type != "LineString":
            continue
        for p in {(round(x, 7), round(y, 7)) for x, y in f.geom.coords}:
            seen[p] = seen.get(p, 0) + 1
    return sorted(p for p, k in seen.items() if k >= 2)


class TmsSampler:
    """Bilinear luma (Pillow 'L') from a geodetic TMS (origin -180/-90, y from the south, 256 px tiles); NaN where a
    tile is missing."""

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

    def _px(self, gx: np.ndarray, gy: np.ndarray) -> np.ndarray:
        """Pixels by global column / row-from-the-south."""
        out = np.full(gx.shape, np.nan)
        tx, ty = gx // 256, gy // 256
        for x, y in set(zip(tx.tolist(), ty.tolist())):
            a = self._tile(x, y)
            if a is None:
                continue
            m = (tx == x) & (ty == y)
            out[m] = a[255 - gy[m] % 256, gx[m] % 256]
        return out

    def luma(self, lon, lat) -> np.ndarray:
        fx = (np.asarray(lon, np.float64) + 180.0) / self.t * 256 - 0.5
        fy = (np.asarray(lat, np.float64) + 90.0) / self.t * 256 - 0.5
        x0, y0 = np.floor(fx).astype(np.int64), np.floor(fy).astype(np.int64)
        wx, wy = fx - x0, fy - y0
        return (
            self._px(x0, y0) * (1 - wx) * (1 - wy)
            + self._px(x0 + 1, y0) * wx * (1 - wy)
            + self._px(x0, y0 + 1) * (1 - wx) * wy
            + self._px(x0 + 1, y0 + 1) * wx * wy
        )


class Local:
    """Equirectangular metres about (lon0, lat0); fine over a 40 km area for 50 m clearances."""

    def __init__(self, lon0: float, lat0: float):
        self.lon0, self.lat0 = lon0, lat0
        self.kx = M_PER_DEG_LAT * math.cos(math.radians(lat0))

    def fwd(self, g):
        return shapely.transform(
            g, lambda c: np.column_stack(((c[:, 0] - self.lon0) * self.kx, (c[:, 1] - self.lat0) * M_PER_DEG_LAT))
        )

    def inv(self, x, y):
        return self.lon0 + np.asarray(x) / self.kx, self.lat0 + np.asarray(y) / M_PER_DEG_LAT


def sample_points(kept, skipped, junction_pts, area, n: int, seed: int, accept, max_tries: int = 200):
    """n points (lon, lat, unit tangent east/north in m, feature) drawn by cumulative length with Random(seed), each at
    least CLEAR_M from a junction, a skipped way and the area edge, and passing accept(lon, lat, tx, ty)."""
    w, s, e, nn = area
    loc = Local((w + e) / 2, (s + nn) / 2)
    lines = [loc.fwd(f.geom) for f in kept]
    cum = np.cumsum([g.length for g in lines])
    obstacles = [loc.fwd(shapely.Point(p)) for p in junction_pts] + [loc.fwd(f.geom) for f in skipped]
    tree = shapely.STRtree(obstacles) if obstacles else None
    inner = loc.fwd(shapely.box(w, s, e, nn)).buffer(-CLEAR_M)
    rng = random.Random(seed)
    out, tries = [], 0
    while len(out) < n and tries < max_tries * n and cum.size:
        tries += 1
        d = rng.uniform(0.0, float(cum[-1]))
        k = int(np.searchsorted(cum, d, side="right"))
        k = min(k, len(lines) - 1)
        g = lines[k]
        dd = d - (cum[k - 1] if k else 0.0)
        p = g.interpolate(dd)
        if not inner.contains(p):
            continue
        if tree is not None and tree.query(p, predicate="dwithin", distance=CLEAR_M).size:
            continue
        a, b = g.interpolate(max(0.0, dd - 2.0)), g.interpolate(min(g.length, dd + 2.0))
        tx, ty = b.x - a.x, b.y - a.y
        norm = math.hypot(tx, ty)
        if norm < 1e-6:
            continue
        lon, lat = loc.inv(p.x, p.y)
        tx, ty = tx / norm, ty / norm
        if not accept(float(lon), float(lat), tx, ty):
            continue
        out.append((float(lon), float(lat), tx, ty, kept[k]))
    return out


def profile_at(sampler: TmsSampler, lon, lat, tx, ty, offsets_m) -> np.ndarray:
    """Luma along the left normal (-ty, tx) at the given offsets."""
    nx, ny = -ty, tx
    kx = M_PER_DEG_LAT * math.cos(math.radians(lat))
    return sampler.luma(lon + offsets_m * nx / kx, lat + offsets_m * ny / M_PER_DEG_LAT)


def save_crop(sampler: TmsSampler, r: dict, path: Path) -> None:
    """Road-aligned crop: columns across the road (-HALF_M..HALF_M, left normal to the right), rows along it (80 m),
    at STEP_M; the centreline red, the match green."""
    across = np.arange(-HALF_M, HALF_M + 1e-9, STEP_M)
    along = np.arange(40.0, -40.0 - 1e-9, -STEP_M)
    tx, ty = r["tangent"]
    nx, ny = -ty, tx
    kx = M_PER_DEG_LAT * math.cos(math.radians(r["lat"]))
    aa, cc = np.meshgrid(along, across, indexing="ij")
    lon = r["lon"] + (aa * tx + cc * nx) / kx
    lat = r["lat"] + (aa * ty + cc * ny) / M_PER_DEG_LAT
    v = np.nan_to_num(sampler.luma(lon.ravel(), lat.ravel()).reshape(aa.shape))
    rgb = np.repeat(np.clip(v, 0, 255).astype(np.uint8)[..., None], 3, axis=2)
    mid = len(across) // 2
    rgb[:, mid] = (255, 0, 0)
    if r["offset_m"] is not None:
        rgb[:, mid + int(round(r["offset_m"] / STEP_M))] = (0, 255, 0)
    Image.fromarray(rgb).resize((rgb.shape[1] * 4, rgb.shape[0] * 4), Image.NEAREST).save(path)


def run(pkg: Path, cache_root: Path | None, n: int, seed: int, crops: Path | None, worst: int) -> dict:
    from camsim_scene.cache import Cache, default_cache_root
    from camsim_scene.features import width_m
    from camsim_scene.manifest import Manifest
    from camsim_scene.pipeline import _load_features, make_context

    m = Manifest.load(pkg / "manifest.json")
    fs = m.layers["landcover"].get("features")
    if not fs or "osm_geofabrik" not in fs["sources"]:
        raise SystemExit(f"{pkg}: the manifest has no OSM feature source")
    feats = _load_features(m, make_context(pkg, m, Cache(cache_root or default_cache_root())))
    roads = [f for f in feats if f.kind == "road"]
    kept, skipped = split_ways(feats)
    jn = junctions(roads)
    sampler = TmsSampler(pkg / "imagery", ZOOM)
    offsets = np.arange(-HALF_M, HALF_M + 1e-9, STEP_M)

    def has_imagery(lon, lat, tx, ty):
        return bool(np.isfinite(profile_at(sampler, lon, lat, tx, ty, offsets)).all())

    pts = sample_points(kept, skipped, jn, tuple(fs["area"]), n, seed, has_imagery)
    rows = []
    for lon, lat, tx, ty, f in pts:
        w = width_m(f)
        off = best_offset(profile_at(sampler, lon, lat, tx, ty, offsets), STEP_M, w)
        rows.append(
            {
                "lon": round(lon, 7),
                "lat": round(lat, 7),
                "tangent": [round(tx, 5), round(ty, 5)],
                "way": f.source_id,
                "highway": f.tags.get("highway"),
                "width_m": w,
                "offset_m": off,
            }
        )
    used = np.array([abs(r["offset_m"]) for r in rows if r["offset_m"] is not None])
    med = float(np.median(used)) if used.size else None
    p90 = float(np.percentile(used, 90)) if used.size else None
    report = {
        "pkg": str(pkg),
        "n_requested": n,
        "seed": seed,
        "n_sampled": len(rows),
        "n_used": int(used.size),
        "n_excluded": len(rows) - int(used.size),
        "median_abs_m": med,
        "p90_abs_m": p90,
        "within_5m": float((used <= 5.0).mean()) if used.size else None,
        "gate_m": GATE_M,
        "pass": med is not None and med <= GATE_M,
        "ways": {"sampled_from": len(kept), "skipped": len(skipped), "junctions": len(jn)},
        "samples": rows,
    }
    if crops:
        crops.mkdir(parents=True, exist_ok=True)
        ranked = sorted((r for r in rows if r["offset_m"] is not None), key=lambda r: -abs(r["offset_m"]))
        for i, r in enumerate(ranked[:worst]):
            save_crop(sampler, r, crops / f"worst{i:02d}_{abs(r['offset_m']):.1f}m_{r['way'].split(':')[1]}.png")
    return report


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--n", type=int, default=200)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--cache", type=Path, help="fetch cache (default .cache/scene or $CAMSIM_SCENE_CACHE)")
    ap.add_argument("--crops", type=Path, help="write PNG crops of the worst profiles here")
    ap.add_argument("--worst", type=int, default=6)
    a = ap.parse_args(argv)
    r = run(a.pkg, a.cache, a.n, a.seed, a.crops, a.worst)
    fmt = lambda v: "n/a" if v is None else f"{v:.2f} m"  # noqa: E731
    print(
        f"road registration: median |offset| {fmt(r['median_abs_m'])}, p90 {fmt(r['p90_abs_m'])}, "
        f"n used {r['n_used']}, excluded {r['n_excluded']} (of {r['n_sampled']} sampled) -> "
        f"{'PASS' if r['pass'] else 'FAIL'} (gate {GATE_M:.0f} m)"
    )
    if a.json:
        a.json.parent.mkdir(parents=True, exist_ok=True)
        a.json.write_text(json.dumps(r, indent=1) + "\n")
    return 0 if r["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
