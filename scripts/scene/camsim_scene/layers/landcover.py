"""CamSim land-cover tiles (ROADMAP 4B format, camsim-landcover-1): 0.05 x 0.05 degree, 600 x 600 8-bit PNGs of
WorldCover class codes, cut losslessly from the 3-degree COGs, plus index.json and ATTRIBUTION.txt.

Tile (lat_index i, lon_index j) covers latitudes [0.05 i, 0.05 (i + 1)) and longitudes [0.05 j, 0.05 (j + 1));
its file is named by its south-west corner (+37.75_-122.45.png); PNG row 0 is the north edge. A COG that
doesn't exist (open ocean) is listed under "missing" and CamSim treats it as no data (code 0)."""

from __future__ import annotations

import json
import math
from collections.abc import Callable
from pathlib import Path

import numpy as np
from PIL import Image

from ..sources.worldcover import (
    ATTRIBUTION,
    CELLS_PER_DEG,
    COG_URL,
    LICENCE,
    SOURCE,
    TILE_DEG,
    TILE_PX,
    cog_name,
    cog_offset,
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


def fetch(
    bbox, out: Path, reader: Reader = read_cog_window, cut_by: str = "scripts/landcover/fetch_worldcover.py"
) -> dict:
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
        f"Source: https://esa-worldcover.org (tiles cut by {cut_by})\n",
        encoding="utf-8",
    )
    return index


def local_reader(paths: dict[str, Path]) -> Reader:
    """Reader over cached COGs keyed by their COG_URL; a URL without a cached file reads as missing (None)."""

    def read(url: str, row_off: int, col_off: int):
        import rasterio
        from rasterio.windows import Window

        p = paths.get(url)
        if p is None:
            return None
        with rasterio.open(p) as ds:
            return ds.read(1, window=Window(col_off, row_off, TILE_PX, TILE_PX))

    return read
