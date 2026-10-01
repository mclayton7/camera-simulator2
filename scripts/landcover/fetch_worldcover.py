# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy", "pillow", "rasterio"]
# ///
"""fetch_worldcover.py — cut ESA WorldCover 2021 v200 into CamSim land-cover tiles (ROADMAP 4B).

Reads only the windows it needs from ESA's 3 x 3 degree Cloud-Optimised GeoTIFFs (HTTP range
reads through rasterio; nothing is downloaded whole) and writes, for every 0.05 x 0.05 degree
tile that touches --bbox, an 8-bit greyscale PNG whose pixel values are the WorldCover class
codes, plus index.json and ATTRIBUTION.txt. A WorldCover cell is 1/12000 degree, so a tile is
exactly 600 x 600 cells: no resampling, the codes pass through losslessly. CamSim reads only
these local files (thermal.land_cover.dir); there is no network access at runtime.

Tile (lat_index i, lon_index j) covers latitudes [0.05 i, 0.05 (i + 1)) and longitudes
[0.05 j, 0.05 (j + 1)). Its file is named by its south-west corner (+37.75_-122.45.png);
PNG row 0 is the north edge, column 0 the west edge. A COG that doesn't exist (open ocean)
is listed under "missing" and CamSim treats it as no data (code 0).

Data: ESA WorldCover 10 m 2021 v200, CC BY 4.0. Attribution (keep it with the data):
  (c) ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021)
  processed by ESA WorldCover consortium

Usage:
    uv run scripts/landcover/fetch_worldcover.py --bbox W S E N [--out DIR]
    # the committed San Francisco sample:
    uv run scripts/landcover/fetch_worldcover.py --bbox -122.56 37.69 -122.35 37.84
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections.abc import Callable
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / "unreal_project/CamSimTest/Content/NonUFS/LandCover"
TILE_DEG = 0.05
CELLS_PER_DEG = 12000
TILE_PX = 600  # TILE_DEG * CELLS_PER_DEG
TILES_PER_COG = 60  # 3 degrees / TILE_DEG
COG_URL = (
    "https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/"
    "ESA_WorldCover_10m_2021_v200_{name}_Map.tif"
)
SOURCE = "ESA WorldCover 10m 2021 v200"
LICENCE = "CC BY 4.0"
ATTRIBUTION = (
    "© ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data (2021) "
    "processed by ESA WorldCover consortium"
)
FORMAT = "camsim-landcover-1"

Reader = Callable[[str, int, int], "np.ndarray | None"]


def tile_range(lo: float, hi: float) -> range:
    """Tile indices k whose [0.05 k, 0.05 (k + 1)) interval intersects [lo, hi)."""
    a = math.floor(round(lo / TILE_DEG, 9))
    b = math.ceil(round(hi / TILE_DEG, 9)) - 1
    return range(a, max(a, b) + 1)


def tiles_for_bbox(w: float, s: float, e: float, n: float) -> list[tuple[int, int]]:
    """(lat_index, lon_index) of every tile touching the bbox, south to north, west to east."""
    if not (w < e and s < n):
        raise ValueError(f"bbox must have W < E and S < N, got {w} {s} {e} {n}")
    if not (-180.0 <= w and e <= 180.0 and -90.0 <= s and n <= 90.0):
        raise ValueError(f"bbox outside [-180, 180] x [-90, 90]: {w} {s} {e} {n}")
    return [(i, j) for i in tile_range(s, n) for j in tile_range(w, e)]


def tile_filename(i: int, j: int) -> str:
    return f"{i * TILE_DEG:+.2f}_{j * TILE_DEG:+.2f}.png"


def cog_name(i: int, j: int) -> str:
    """The WorldCover COG holding tile (i, j), named by its 3-degree SW corner (N36W123)."""
    lat = (i // TILES_PER_COG) * 3
    lon = (j // TILES_PER_COG) * 3
    return f"{'N' if lat >= 0 else 'S'}{abs(lat):02d}{'E' if lon >= 0 else 'W'}{abs(lon):03d}"


def cog_offset(i: int, j: int) -> tuple[int, int]:
    """(row_off, col_off) in cells of tile (i, j) inside its COG (row 0 = the COG's north edge)."""
    row = (i // TILES_PER_COG) * TILES_PER_COG + TILES_PER_COG - 1 - i
    col = j - (j // TILES_PER_COG) * TILES_PER_COG
    return row * TILE_PX, col * TILE_PX


def read_cog_window(url: str, row_off: int, col_off: int) -> np.ndarray | None:
    """TILE_PX x TILE_PX uint8 codes from one COG (HTTP range reads), or None when the COG doesn't exist."""
    import rasterio
    from rasterio.errors import RasterioIOError
    from rasterio.windows import Window

    try:
        with rasterio.open(url) as ds:
            if ds.width != 3 * CELLS_PER_DEG or ds.height != 3 * CELLS_PER_DEG or ds.dtypes[0] != "uint8":
                raise SystemExit(f"{url}: unexpected layout {ds.width}x{ds.height} {ds.dtypes[0]}")
            return ds.read(1, window=Window(col_off, row_off, TILE_PX, TILE_PX))
    except RasterioIOError as e:
        if "404" in str(e):
            return None
        raise


def write_tile(codes: np.ndarray, path: Path) -> None:
    if codes.shape != (TILE_PX, TILE_PX) or codes.dtype != np.uint8:
        raise ValueError(f"tile must be {TILE_PX}x{TILE_PX} uint8, got {codes.shape} {codes.dtype}")
    Image.fromarray(codes).save(path, format="PNG", optimize=True)


def build_index(bbox, written: list[tuple[int, int]], missing: list[tuple[int, int]]) -> dict:
    return {
        "format": FORMAT,
        "source": SOURCE,
        "source_url": COG_URL,
        "licence": LICENCE,
        "attribution": ATTRIBUTION,
        "tile_deg": TILE_DEG,
        "tile_px": TILE_PX,
        "bbox": [float(v) for v in bbox],
        "tiles": [{"file": tile_filename(i, j), "lat_index": i, "lon_index": j} for i, j in written],
        "missing": [[i, j] for i, j in missing],
    }


def fetch(bbox, out: Path, reader: Reader = read_cog_window) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    written: list[tuple[int, int]] = []
    missing: list[tuple[int, int]] = []
    for i, j in tiles_for_bbox(*bbox):
        row, col = cog_offset(i, j)
        codes = reader(COG_URL.format(name=cog_name(i, j)), row, col)
        if codes is None:
            missing.append((i, j))
            continue
        write_tile(np.ascontiguousarray(codes, dtype=np.uint8), out / tile_filename(i, j))
        written.append((i, j))
    index = build_index(bbox, written, missing)
    (out / "index.json").write_text(json.dumps(index, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    (out / "ATTRIBUTION.txt").write_text(
        f"{SOURCE}\n{ATTRIBUTION}\nLicence: {LICENCE} (https://creativecommons.org/licenses/by/4.0/)\n"
        "Source: https://esa-worldcover.org (tiles cut by scripts/landcover/fetch_worldcover.py)\n",
        encoding="utf-8",
    )
    return index


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bbox", nargs=4, type=float, metavar=("W", "S", "E", "N"), required=True)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    a = ap.parse_args(argv)
    index = fetch(tuple(a.bbox), a.out)
    print(f"{len(index['tiles'])} tiles written, {len(index['missing'])} missing -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
