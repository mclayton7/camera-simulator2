"""Choose the dense_pass path on a scene package (REALISM R3/R4 bench) and write scripts/bench/dense_path.json.

    uv run --project scripts/scene python scripts/bench/select_dense_path.py PKG [--out scripts/bench/dense_path.json]

Rule (fixed before any bench run): candidates from bench.dense_path.candidates (9 km straight segments starting on a
1 km grid, bearings every 15 deg, 500 m inside the package bbox, ranked by tree cover + built-up land in the swept
strip); altitude = terrain at the start + 300 m (package quantized-mesh, z12, max vertex within 250 m); the first of the
top 50 whose terrain (sampled every 500 m) never rises more than 150 m above the start wins.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bench import dense_path as dp  # noqa: E402


def terrain_max_height(
    pkg: Path, lon: float, lat: float, radius_m: float = 250.0, z: int = 12
) -> float | None:
    from camsim_scene import qmesh

    d = 180.0 / 2**z
    x, y = int((lon + 180.0) // d), int((lat + 90.0) // d)
    p = Path(pkg) / "terrain" / str(z) / str(x) / f"{y}.terrain"
    if not p.exists():
        return None
    q = qmesh.decode(p.read_bytes())
    w, s = -180.0 + x * d, -90.0 + y * d
    vl, vt = q.lonlat((w, s, w + d, s + d))
    h = q.heights()
    kx = dp.M_PER_DEG * math.cos(math.radians(lat))
    dist = np.hypot((vl - lon) * kx, (vt - lat) * dp.M_PER_DEG)
    near = dist <= radius_m
    return float(h[near].max() if near.any() else h[np.argmin(dist)])


def choose(
    pkg: Path,
    grid: dp.Grid,
    ranked: list[dp.Candidate],
    length_m: float = 9000.0,
    agl_m: float = 300.0,
    max_rise_m: float = 150.0,
    top_n: int = 50,
) -> dict | None:
    for rank, c in enumerate(ranked[:top_n]):
        b = math.radians(c.bearing_deg)
        hs = []
        for a in np.arange(0.0, length_m + 1.0, 500.0):
            lon, lat = dp.xy_to_lonlat(grid, c.x0 + a * math.sin(b), c.y0 + a * math.cos(b))
            hs.append(terrain_max_height(pkg, lon, lat))
        if any(h is None for h in hs) or max(hs) - hs[0] > max_rise_m:
            continue
        lon0, lat0 = dp.xy_to_lonlat(grid, c.x0, c.y0)
        return {
            "start_lat": round(lat0, 6),
            "start_lon": round(lon0, 6),
            "bearing_deg": c.bearing_deg,
            "alt_hae_m": round(hs[0] + agl_m, 1),
            "length_m": length_m,
            "speed_mps": 100.0,
            "gimbal_pitch_deg": -20.0,
            "selector": {
                "package": Path(pkg).name,
                "tree": round(c.tree, 2),
                "built": round(c.built, 2),
                "score": round(c.score, 4),
                "rank": rank,
                "terrain_start_m": round(hs[0], 1),
                "terrain_max_m": round(max(hs), 1),
            },
        }
    return None


def main(argv: list[str] | None = None) -> int:
    from camsim_scene.manifest import Manifest

    ap = argparse.ArgumentParser()
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--out", type=Path, default=Path(__file__).with_name("dense_path.json"))
    a = ap.parse_args(argv)
    m = Manifest.load(a.pkg / "manifest.json")
    grid = dp.class_grid(dp.LandCover(a.pkg / "landcover"), tuple(m.bbox))
    ranked = dp.candidates(grid)
    print(f"[select] {len(ranked)} candidates with both tree cover and built-up land")
    best = choose(a.pkg, grid, ranked)
    if best is None:
        print("[select] no candidate passed the terrain-rise rule", file=sys.stderr)
        return 1
    a.out.write_text(json.dumps(best, indent=2) + "\n")
    print(json.dumps(best, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
