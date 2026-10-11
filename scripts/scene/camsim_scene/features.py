"""Vector features burned into the land-cover tiles (REALISM R2, spec docs/superpowers/specs/2026-10-10-r2-vector-
features-design.md, sections 1, 3, 4). Each feature becomes a polygon in a local metric frame per tile (equirectangular
about the tile centre), at least one cell (9.2767 m) wide, rasterised with the centre-sample rule in priority order
(water < paved areas < buildings (R3b, from the footprint raster) < rail < roads; later wins) and sorted by source id within a priority, so output is deterministic.
Bridges, tunnels and underground features are skipped (no deck geometry before R3)."""

from __future__ import annotations

import math
import re
from dataclasses import dataclass

import numpy as np
from shapely import affinity
from shapely.geometry import box

from .sources.worldcover import CELLS_PER_DEG, TILE_DEG, TILE_PX

# Bump on any change in features.py, osm_geofabrik.read_osm or nhd_hr.read_nhd: the derived-features cache key (pipeline.py)
# covers this, not the reader code.
RULES_VERSION = "1"
M_PER_DEG_LAT = 111_320.0
CELL_M = (
    M_PER_DEG_LAT / CELLS_PER_DEG
)  # 9.2767 m: a cell's N-S side, its longer side at every latitude (E-W shrinks by cos lat)
MIN_W_M = (
    1.05 * CELL_M
)  # minimum burned width: a strip this wide covers a cell centre in every cross-section, with margin
CODES = {200: "paved_asphalt", 201: "paved_concrete", 210: "rail", 220: "inland_water", 230: "building"}
BUILDING_CODE = 230  # REALISM R3b: = the CIGI material code of a building hit (Thermal/LandCoverLookup.h)
# Burn order, later wins. "building" is the R3b footprint burn (not a Feature kind); the feature kinds keep R2's order.
PRIORITY = {"water": 0, "paved_area": 1, "building": 2, "rail": 3, "road": 4}
LANE_M = 3.5
SHOULDER_M = {"motorway": 3.0, "trunk": 3.0}
RUNWAY_M = {"runway": 45.0, "taxiway": 23.0}
TRACK_M = 4.0
CONCRETE = {"concrete", "concrete:plates", "concrete:lanes"}
UNPAVED = {"unpaved", "gravel", "dirt", "ground", "compacted", "fine_gravel", "sand", "grass", "earth", "mud"}


@dataclass(frozen=True)
class Feature:
    kind: str  # "road" | "paved_area" | "rail" | "water"
    source_id: str  # "osm:w123", "osm:a456", "nhd:<permanent id>"
    geom: object  # shapely geometry in lon/lat (LineString / Polygon / MultiPolygon)
    tags: dict  # the tags the rules read (str -> str)


def _num(v: str | None) -> float | None:
    if v is None:
        return None
    m = re.match(r"^\s*([0-9]+(?:\.[0-9]+)?)\s*m?\s*$", v)
    return float(m.group(1)) if m else None


def _skipped(t: dict) -> bool:
    if t.get("bridge", "no") != "no" or t.get("tunnel", "no") != "no":
        return True
    if t.get("covered") == "yes" or t.get("location") == "underground":
        return True
    try:
        return float(t.get("layer", "0")) < 0
    except ValueError:
        return False


def width_m(f: Feature) -> float:
    t = f.tags
    if f.kind == "rail":
        tracks = _num(t.get("tracks")) or 1.0
        return TRACK_M * tracks
    w = _num(t.get("width"))
    if w:
        return w
    if t.get("aeroway") in RUNWAY_M:
        return RUNWAY_M[t["aeroway"]]
    hw = t.get("highway", "").removesuffix("_link")
    lanes = _num(t.get("lanes"))
    if not lanes or lanes != int(lanes):
        lanes = 2.0 if hw in ("motorway", "trunk") else (1.0 if t.get("oneway") == "yes" else 2.0)
    return lanes * LANE_M + SHOULDER_M.get(hw, 1.0)


def code_for(f: Feature) -> int | None:
    if f.kind == "water":
        return 220
    t = f.tags
    if _skipped(t):
        return None
    if f.kind == "rail":
        return 210
    s = t.get("surface")
    if s in UNPAVED:
        return None
    if s in CONCRETE or (s is None and t.get("aeroway") == "runway"):
        return 201
    return 200


def _to_local(lat0: float, lon0: float):
    kx = M_PER_DEG_LAT * math.cos(math.radians(lat0))
    return lambda g: affinity.affine_transform(g, [kx, 0, 0, M_PER_DEG_LAT, -lon0 * kx, -lat0 * M_PER_DEG_LAT])


def _polygon(f: Feature, local) -> object:
    """Lines (roads, rail, runways) are buffered to max(width, MIN_W_M); polygons (water, paved areas) are burned as they
    are: a polygon arm narrower than a cell may vanish, which is the 10 m raster's limit, not widened."""
    g = local(f.geom)
    if g.geom_type in ("LineString", "MultiLineString"):
        return g.buffer(max(width_m(f), MIN_W_M) / 2.0, cap_style="flat", join_style="round")
    if g.geom_type in ("Polygon", "MultiPolygon"):
        return g
    return None


def _tile_frame(i: int, j: int):
    """The tile's local metric frame (equirectangular about its centre), metres per degree of longitude, and the
    raster transform of its TILE_PX x TILE_PX cells (north-west corner, local metres)."""
    from rasterio.transform import Affine

    s, w = i * TILE_DEG, j * TILE_DEG
    lat0, lon0 = s + TILE_DEG / 2.0, w + TILE_DEG / 2.0
    kx = M_PER_DEG_LAT * math.cos(math.radians(lat0))
    cell_x, cell_y = kx / CELLS_PER_DEG, M_PER_DEG_LAT / CELLS_PER_DEG
    x0, y0 = (w - lon0) * kx, (s + TILE_DEG - lat0) * M_PER_DEG_LAT
    return _to_local(lat0, lon0), kx, Affine(cell_x, 0, x0, 0, -cell_y, y0)


def _feature_shapes(i: int, j: int, feats: list[Feature], area, local, kx: float) -> list[tuple]:
    """(local polygon, code, priority) of every feature burned in tile (i, j) within `area`, in burn order."""
    s, w = i * TILE_DEG, j * TILE_DEG
    tile = box(w, s, w + TILE_DEG, s + TILE_DEG)
    inside = tile.intersection(box(*area))
    clip = local(inside)
    if clip.is_empty:
        return []
    cw, cs, ce, cn = inside.bounds
    shapes = []
    for f in sorted(feats, key=lambda f: (PRIORITY[f.kind], f.source_id)):
        code = code_for(f)
        if code is None:
            continue
        # cheap early-out on raw lon/lat bounds, before any transform or buffer
        reach = (max(width_m(f), MIN_W_M) / 2.0 if f.kind in ("road", "rail") else 0.0) + 1.0
        fw, fs, fe, fn = f.geom.bounds
        dlat, dlon = reach / M_PER_DEG_LAT, reach / kx
        if fe + dlon < cw or fw - dlon > ce or fn + dlat < cs or fs - dlat > cn:
            continue
        p = _polygon(f, local)
        if p is None or p.is_empty:
            continue
        p = p.intersection(clip)
        if p.is_empty:
            continue
        shapes.append((p, code, PRIORITY[f.kind]))
    return shapes


def _rasterise_into(codes: np.ndarray, shapes: list[tuple], transform) -> None:
    """One rasterize pass; later shapes overwrite earlier ones. Codes are >= 200, so 0 means untouched."""
    from rasterio import features as rf

    if not shapes:
        return
    out = rf.rasterize(
        [(p, c) for p, c, _ in shapes], out_shape=(TILE_PX, TILE_PX), transform=transform, fill=0,
        all_touched=False, dtype="uint8",
    )
    m = out > 0
    codes[m] = out[m]


def burn_tile(codes: np.ndarray, i: int, j: int, feats: list[Feature], area, buildings: np.ndarray | None = None) -> int:
    """Burn `feats` (within `area`) into tile (i, j)'s codes in priority order; with `buildings` (REALISM R3b, a
    (TILE_PX, TILE_PX) bool from footprints.burn_cells) those cells become BUILDING_CODE after water and paved areas and
    before rail and roads. buildings=None is R2's single pass, byte for byte. Returns the number of changed cells."""
    local, kx, transform = _tile_frame(i, j)
    before = codes.copy()
    shapes = _feature_shapes(i, j, feats, area, local, kx) if feats and area is not None else []
    if buildings is None:
        _rasterise_into(codes, shapes, transform)
    else:
        b = PRIORITY["building"]
        _rasterise_into(codes, [x for x in shapes if x[2] < b], transform)
        codes[buildings] = BUILDING_CODE
        _rasterise_into(codes, [x for x in shapes if x[2] > b], transform)
    return int((codes != before).sum())
