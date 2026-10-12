#!/usr/bin/env python3
# Copyright CamSim Contributors. All Rights Reserved.
"""Acceptance helper (REALISM R3b): CIGI probe points for land-cover code 230 on terrain. Prints the centre of the burned
(230) cell nearest --near, and a point --gap-m beyond that cell's edge (west, east, north or south: the first whose cell
is not 230) with that cell's code. Query them with CAMSIM_SCENE_BUILDINGS_ENABLED=0, so HAT/HOT hits the terrain:

  uv run --project scripts/scene python scripts/scene/tools/burn_probe_points.py "$PKG" --near 33.3000,-117.3175
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image

from camsim_scene.layers.landcover import tile_filename

TILE_DEG, TILE_PX = 0.05, 600
CELL_DEG = TILE_DEG / TILE_PX
M_PER_DEG = 111_320.0
_TILES: dict[tuple[str, int, int], np.ndarray | None] = {}


def _tile(lc_dir: Path, i: int, j: int) -> np.ndarray | None:
    key = (str(lc_dir), i, j)
    if key not in _TILES:
        p = Path(lc_dir) / tile_filename(i, j)
        _TILES[key] = np.asarray(Image.open(p)) if p.exists() else None
    return _TILES[key]


def _cell(lat: float, lon: float) -> tuple[int, int, int, int]:
    i, j = math.floor(round(lat / TILE_DEG, 9)), math.floor(round(lon / TILE_DEG, 9))
    row = min(TILE_PX - 1, max(0, math.floor(((i + 1) * TILE_DEG - lat) / CELL_DEG)))
    col = min(TILE_PX - 1, max(0, math.floor((lon - j * TILE_DEG) / CELL_DEG)))
    return i, j, row, col


def code_at(lc_dir: Path, lat: float, lon: float) -> int:
    i, j, row, col = _cell(lat, lon)
    t = _tile(lc_dir, i, j)
    return int(t[row, col]) if t is not None else 0


def nearest_burned(lc_dir: Path, lat: float, lon: float, search_cells: int = 60) -> tuple[float, float] | None:
    """Centre of the 230 cell nearest (lat, lon) within search_cells (Chebyshev) in the same tile."""
    i, j, row, col = _cell(lat, lon)
    t = _tile(lc_dir, i, j)
    if t is None:
        return None
    r0, r1 = max(0, row - search_cells), min(TILE_PX, row + search_cells + 1)
    c0, c1 = max(0, col - search_cells), min(TILE_PX, col + search_cells + 1)
    rr, cc = np.nonzero(t[r0:r1, c0:c1] == 230)
    if len(rr) == 0:
        return None
    k = int(np.argmin((rr + r0 - row) ** 2 + (cc + c0 - col) ** 2))
    r, c = int(rr[k] + r0), int(cc[k] + c0)
    return (i + 1) * TILE_DEG - (r + 0.5) * CELL_DEG, j * TILE_DEG + (c + 0.5) * CELL_DEG


def outside_point(lc_dir: Path, lat: float, lon: float, gap_m: float = 10.0) -> tuple[float, float, int, str] | None:
    """A point gap_m beyond the edge of the cell centred at (lat, lon), in the first direction whose cell is not 230."""
    kx = M_PER_DEG * math.cos(math.radians(lat))
    half_n, half_e = CELL_DEG / 2.0 * M_PER_DEG, CELL_DEG / 2.0 * kx
    for name, de, dn in (("W", -(half_e + gap_m), 0.0), ("E", half_e + gap_m, 0.0), ("N", 0.0, half_n + gap_m),
                         ("S", 0.0, -(half_n + gap_m))):
        plat, plon = lat + dn / M_PER_DEG, lon + de / kx
        code = code_at(lc_dir, plat, plon)
        if code != 230:
            return plat, plon, code, name
    return None


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--near", required=True, help="LAT,LON")
    ap.add_argument("--gap-m", type=float, default=10.0)
    a = ap.parse_args()
    lat, lon = (float(v) for v in a.near.split(","))
    lc = a.pkg / "landcover"
    inside = nearest_burned(lc, lat, lon)
    if inside is None:
        raise SystemExit(f"no 230 cell within 60 cells of {a.near}")
    out = outside_point(lc, *inside, gap_m=a.gap_m)
    if out is None:
        raise SystemExit("every neighbour direction is 230 too: pick another --near")
    print(json.dumps({"inside": [round(v, 7) for v in inside], "outside": [round(out[0], 7), round(out[1], 7)],
                      "outside_code": out[2], "direction": out[3]}))


if __name__ == "__main__":
    main()
