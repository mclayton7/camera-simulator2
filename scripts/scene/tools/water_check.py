"""R1 water gate (spec section 4 and acceptance 5): over open water, built imagery leaves must hold the raw decode of
the sources behind NAIP (no NAIP, no colour match) and be as uniform as it; with --before-hashes (hashes.txt of the
same package built without the water mask), no leaf without water on its lattice may have changed.

    uv run --project scripts/scene python scripts/scene/tools/water_check.py PKG --cache .cache/scene \
        [--before-hashes OLD/hashes.txt] [--samples 200] [--min-zoom 10]

Open water: WorldCover class 80 further than max(naip_water_buffer_m, water_fade_m) + 2 lattice nodes from land.
Prints a JSON report; exits 1 when a gate fails, 2 when the package has no water mask."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

from camsim_scene.cache import Cache
from camsim_scene.context import WorkerState, coverage
from camsim_scene.layers import imagery
from camsim_scene.manifest import Manifest, read_hashes
from camsim_scene.pipeline import make_context
from camsim_scene.sources.base import M_PER_DEG
from camsim_scene.tiling import plan_tiles, split_keys, tile_size_deg
from camsim_scene.water import WATER, class_codes

MEAN_DN = 1.5  # pooled mean |built - raw| per band
TILE_P95_DN = 3.0  # 95th percentile over tiles of the per-tile mean |built - raw|, per band
STD_DN = 1.0  # median over tiles of (G std built - G std raw): no streaks or blocks
MIN_PX = 4096  # open-water pixels for a tile to count


def wet_leaves(st, keys, z: int, n: int):
    """Up to n leaves (evenly strided over the plan's order) whose centre is WorldCover water."""
    x, y = split_keys(keys)
    t = tile_size_deg(z)
    wet = class_codes(st.water.classes, (x + 0.5) * t - 180.0, (y + 0.5) * t - 90.0) == WATER
    out = list(zip(x[wet].tolist(), y[wet].tolist()))
    return out[:: max(1, len(out) // n)][:n]


def tile_stats(pkg: Path, st, z: int, x: int, y: int) -> dict | None:
    feather = st.balance.feather_m
    dist = imagery.land_distance(z, x, y, st.water, feather)
    if dist is None:
        return None
    reach = max(st.water.buffer_m, st.water.fade_m) + 2 * imagery.feather_spacing_deg(z) * M_PER_DEG
    sea = imagery.pixel_distance(z, x, y, dist, feather) > reach
    if sea.sum() < MIN_PX:
        return None
    entries = st.index["imagery"].query(imagery.query_bounds(z, x, y))
    raw = imagery.leaf_rgb(z, x, y, [e for e in entries if imagery.source_of(e) != st.balance.reference])
    img = Image.open(pkg / "imagery" / str(z) / str(x) / f"{y}.jpg").convert("RGB")
    built = np.moveaxis(np.asarray(img), -1, 0).astype(np.float64)[:, sea]
    raw = raw.astype(np.float64)[:, sea]
    return {
        "n": int(sea.sum()),
        "mean_abs": np.abs(built - raw).mean(axis=1),
        "built": built.mean(axis=1),
        "raw": raw.mean(axis=1),
        "std_excess": float(built[1].std() - raw[1].std()),
    }


def sampling_failures(report: dict, lo: int, hi: int) -> list[str]:
    """Record the zooms lo..hi with no sampled open-water tile; nothing sampled at all is a failure, not a pass."""
    report["zooms_unsampled"] = [z for z in range(lo, hi + 1) if str(z) not in report["zooms"]]
    return [] if report["zooms"] else ["no open-water leaves sampled"]


def r2(v) -> list[float]:
    return [round(float(x), 2) for x in v]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--cache", type=Path, required=True)
    ap.add_argument("--before-hashes", type=Path)
    ap.add_argument("--samples", type=int, default=200, help="open-water leaves per zoom")
    ap.add_argument("--min-zoom", type=int, default=10, help="skip coarser leaves (z8 reads ~70 MB of codes each)")
    a = ap.parse_args()
    m = Manifest.load(a.pkg / "manifest.json")
    st = WorkerState(make_context(a.pkg, m, Cache(a.cache)))
    if st.balance is None or st.water is None:
        print(f"{a.pkg}: no water mask (no balance.json, no WorldCover, or no water settings)", file=sys.stderr)
        return 2
    if st.water.buffer_m == 0:
        print(f"{a.pkg}: naip_water_buffer_m = 0 (water clip off); the gate would fail by design", file=sys.stderr)
        return 2
    plan = plan_tiles(m.region_objs(), "imagery", coverage(m, "imagery"))
    report, fails = {"zooms": {}}, []
    for z in range(a.min_zoom, plan.max_zoom + 1):
        if not len(plan.leaves[z]):
            continue
        rows = [r for x, y in wet_leaves(st, plan.leaves[z], z, a.samples) if (r := tile_stats(a.pkg, st, z, x, y))]
        if not rows:
            continue
        n = np.array([r["n"] for r in rows], np.float64)[:, None]
        mean_abs = np.stack([r["mean_abs"] for r in rows])
        pooled, p95 = (mean_abs * n).sum(0) / n.sum(), np.percentile(mean_abs, 95, axis=0)
        excess = float(np.median([r["std_excess"] for r in rows]))
        report["zooms"][str(z)] = {
            "tiles": len(rows),
            "pooled_mean_abs_dn": r2(pooled),
            "tile_p95_mean_abs_dn": r2(p95),
            "mean_built_dn": r2((np.stack([r["built"] for r in rows]) * n).sum(0) / n.sum()),
            "mean_raw_dn": r2((np.stack([r["raw"] for r in rows]) * n).sum(0) / n.sum()),
            "median_g_std_excess_dn": round(excess, 2),
        }
        if (pooled > MEAN_DN).any() or (p95 > TILE_P95_DN).any() or excess > STD_DN:
            fails.append(f"open water z{z}")
    fails += sampling_failures(report, a.min_zoom, plan.max_zoom)
    if a.before_hashes:
        old = {}
        for line in a.before_hashes.read_text(encoding="utf-8").splitlines():
            h, rel = line.split("  ", 1)
            old[rel] = h
        new = read_hashes(a.pkg)
        changed, unexpected = 0, []
        for z in range(plan.max_zoom + 1):
            x, y = split_keys(plan.leaves[z])
            for xx, yy in zip(x.tolist(), y.tolist()):
                rel = f"imagery/{z}/{xx}/{yy}.jpg"
                if rel in old and old[rel] != new.get(rel):
                    changed += 1
                    if imagery.land_distance(z, xx, yy, st.water, st.balance.feather_m) is None:
                        unexpected.append(rel)
        report["leaves_changed"] = changed
        report["changed_without_water"] = {"count": len(unexpected), "first": unexpected[:20]}
        if unexpected:
            fails.append("leaves without water changed")
    report["fails"] = fails
    print(json.dumps(report, indent=1, sort_keys=True))
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
