# /// script
# requires-python = ">=3.12"
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

The code lives in scripts/scene (camsim_scene.layers.landcover and camsim_scene.sources.worldcover), which
also builds the land-cover layer of scene packages; this script keeps the standalone CLI and output.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts" / "scene"))

from camsim_scene.layers.landcover import (  # noqa: E402,F401
    FORMAT,
    Reader,
    build_index,
    fetch,
    read_cog_window,
    tile_filename,
    tile_range,
    tiles_for_bbox,
    write_tile,
)
from camsim_scene.sources.worldcover import (  # noqa: E402,F401
    ATTRIBUTION,
    CELLS_PER_DEG,
    COG_URL,
    LICENCE,
    SOURCE,
    TILE_DEG,
    TILE_PX,
    TILES_PER_COG,
    cog_name,
    cog_offset,
)

DEFAULT_OUT = REPO / "unreal_project/CamSimTest/Content/NonUFS/LandCover"


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
