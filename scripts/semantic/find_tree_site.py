#!/usr/bin/env python3
"""Pick the semantic_check tree site (ROADMAP 4D): the centre of the land-cover cell nearest the georeg flat site whose
NxN neighbourhood is all WorldCover tree cover (code 10). Prints the JSON entry for scripts/semantic/sites_pendleton.json.

    uv run --project scripts/scene python scripts/semantic/find_tree_site.py .cache/scene-packages/pendleton-bridges [N]
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scene" / "tools"))
import burn_probe_points as bpp  # noqa: E402

FLAT = (33.3072917, -117.344625)


def main() -> None:
    pkg, n = Path(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 9
    lc = pkg / "landcover"
    i0, j0, _, _ = bpp._cell(*FLAT)
    best = None
    for di in range(-3, 4):
        for dj in range(-3, 4):
            t = bpp._tile(lc, i0 + di, j0 + dj)
            if t is None:
                continue
            tree = (t == 10).astype(np.int32)
            s = np.pad(tree, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
            h = n // 2
            for r in range(h, bpp.TILE_PX - h):
                win = s[r + h + 1, n:] - s[r - h, n:] - s[r + h + 1, :-n] + s[r - h, :-n]
                for c in np.nonzero(win == n * n)[0]:
                    col = int(c) + h
                    lat = (i0 + di + 1) * bpp.TILE_DEG - (r + 0.5) * bpp.CELL_DEG
                    lon = (j0 + dj) * bpp.TILE_DEG + (col + 0.5) * bpp.CELL_DEG
                    d = math.hypot((lat - FLAT[0]) * bpp.M_PER_DEG, (lon - FLAT[1]) * bpp.M_PER_DEG * math.cos(math.radians(lat)))
                    if best is None or d < best[0]:
                        best = (d, lat, lon)
    if best is None:
        sys.exit(f"no {n}x{n} tree-cover patch within 3 tiles of the flat site")
    print(json.dumps({"name": "tree", "lat": round(best[1], 7), "lon": round(best[2], 7),
                      "why": f"{n}x{n} land-cover cells all WorldCover 10 (tree cover), nearest such patch to the georeg "
                             f"flat site ({best[0] / 1000:.1f} km); find_tree_site.py"}, indent=1))


if __name__ == "__main__":
    main()
