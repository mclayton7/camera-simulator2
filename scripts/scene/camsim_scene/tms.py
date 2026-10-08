"""TMS `tilemapresource.xml` (geodetic profile, EPSG:4326, origin -180/-90, 256 px JPEG tiles).
For file:// URLs Cesium's TMS overlay must point at this file itself (it appends nothing)."""

from __future__ import annotations

import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

import numpy as np

from .tiling import Bounds, LayerPlan, split_keys, tile_size_deg


def tilemapresource_xml(title: str, max_zoom: int, bounds: Bounds) -> str:
    sets = "".join(
        f'<TileSet href="{z}" units-per-pixel="{180.0 / 256 / (1 << z):.16g}" order="{z}"/>'
        for z in range(max_zoom + 1)
    )
    w, s, e, n = bounds
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<TileMap version="1.0.0" tilemapservice="http://tms.osgeo.org/1.0.0">'
        f"<Title>{escape(title)}</Title><Abstract/><SRS>EPSG:4326</SRS>"
        f'<BoundingBox minx="{w:.10f}" miny="{s:.10f}" maxx="{e:.10f}" maxy="{n:.10f}"/>'
        '<Origin x="-180" y="-90"/>'
        '<TileFormat width="256" height="256" mime-type="image/jpeg" extension="jpg"/>'
        f'<TileSets profile="geodetic">{sets}</TileSets></TileMap>\n'
    )


def parse_tilemapresource(text: str) -> tuple[list[int], Bounds]:
    root = ET.fromstring(text)
    bb = root.find("BoundingBox")
    bounds = tuple(float(bb.get(k)) for k in ("minx", "miny", "maxx", "maxy"))
    levels = sorted(int(t.get("href")) for t in root.find("TileSets").findall("TileSet"))
    return levels, bounds


def plan_bounds(plan: LayerPlan) -> Bounds:
    """Union of the bounds of every tile in the plan."""
    w = s = 180.0
    e = n = -180.0
    for z, keys in plan.tiles.items():
        if len(keys) == 0:
            continue
        x, y = split_keys(keys)
        t = tile_size_deg(z)
        w = min(w, float(-180.0 + np.min(x) * t))
        e = max(e, float(-180.0 + (np.max(x) + 1) * t))
        s = min(s, float(-90.0 + np.min(y) * t))
        n = max(n, float(-90.0 + (np.max(y) + 1) * t))
    return (w, s, e, n)
