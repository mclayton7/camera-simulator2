"""Buildings layer (REALISM R3): Overture footprints -> extruded flat-roof meshes -> 3D Tiles 1.1."""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import shapely
import shapely.affinity
from shapely.geometry.polygon import orient

from . import geodesy
from .gltf import PropertyTable, TileMesh
from .roofs import RoofOptions, RoofSampler, roof_texture
from .tiling import tile_bounds, tile_size_deg

# bump when geometry/height/tier rules change (it is in the layer settings hash). 2: R3b footprint mask + roof atlas
BUILDINGS_RULES_VERSION = "2"
DEFAULT_RELEASE = "2026-09-23.1"
TIER_MIN_AREA_M2 = {13: 2000.0, 14: 300.0}  # else LEAF_ZOOM
LEAF_ZOOM = 15
GEOMETRIC_ERROR = {"root": 200.0, 13: 60.0, 14: 20.0, 15: 6.0}

M_PER_DEG = 111320.0
FLOOR_M = 3.2
DEFAULT_HEIGHT_M = 4.0
MIN_HEIGHT_M, MAX_HEIGHT_M = 2.5, 150.0
SINK_M = 0.5
MIN_EDGE_M = 0.01


@dataclass(frozen=True)
class Footprint:
    source_id: str
    geometry: object  # shapely, lon/lat
    height: float | None
    num_floors: int | None
    cls: str


@dataclass
class Building:
    source_id: str
    parts: list  # shapely Polygons, lon/lat, exterior CCW / holes CW
    cls: str
    height_m: float
    height_source: int  # index into gltf.HEIGHT_SOURCES
    base_m: float
    roof_m: float
    area_m2: float
    tile: tuple[int, int, int]  # (z, x, y)


@dataclass
class TileGeometry:
    positions: np.ndarray  # (n, 3) float32: east, up, -north (m) from the tile's ENU origin
    normals: np.ndarray  # (n, 3) float32
    feature_ids: np.ndarray  # (n,) float32
    roof: dict  # feature index -> its roof vertex indices (uint32, ascending; consecutive triples are triangles)
    wall_indices: np.ndarray  # uint32
    table: PropertyTable
    matrix: np.ndarray  # 4 x 4 ENU -> ECEF of the tile origin


def read_snapshot(path: Path) -> list[Footprint]:
    from .sources.overture_buildings import connect

    con = connect()
    try:
        rows = con.execute(
            "SELECT id, geometry, height, num_floors, class FROM read_parquet(?) ORDER BY id", [str(path)]
        ).fetchall()
    finally:
        con.close()
    out, seen = [], set()
    for sid, wkb, height, floors, cls in rows:
        if sid in seen or wkb is None:
            continue
        seen.add(sid)
        out.append(Footprint(sid, shapely.from_wkb(bytes(wkb)), height, floors, cls or ""))
    return out


def local_scale(lat0: float) -> tuple[float, float]:
    return M_PER_DEG * math.cos(math.radians(lat0)), M_PER_DEG


def _area_m2(poly, lat0: float) -> float:
    sx, sy = local_scale(lat0)
    return abs(poly.area) * sx * sy


def clean(geom, min_area_m2: float) -> list:
    if geom is None or geom.is_empty:
        return []
    g = shapely.make_valid(geom)
    polys = [p for p in shapely.get_parts(g) if isinstance(p, shapely.Polygon)]
    multi = [q for p in shapely.get_parts(g) if isinstance(p, shapely.MultiPolygon) for q in p.geoms]
    lat0 = geom.centroid.y if not geom.centroid.is_empty else 0.0
    out = []
    for p in sorted(polys + multi, key=lambda p: (p.bounds, p.area)):
        if not p.is_empty and _area_m2(p, lat0) >= min_area_m2:
            out.append(orient(p, sign=1.0))
    return out


def building_height(height, num_floors) -> tuple[float, int]:
    if height is not None and math.isfinite(height) and height > 0:
        h, src = float(height), 0
    elif num_floors is not None and num_floors > 0:
        h, src = num_floors * FLOOR_M, 1
    else:
        h, src = DEFAULT_HEIGHT_M, 2
    return min(MAX_HEIGHT_M, max(MIN_HEIGHT_M, h)), src


def tier_zoom(area_m2: float) -> int:
    for z in sorted(TIER_MIN_AREA_M2):
        if area_m2 >= TIER_MIN_AREA_M2[z]:
            return z
    return LEAF_ZOOM


def tile_of(lon: float, lat: float, z: int) -> tuple[int, int]:
    s = tile_size_deg(z)
    return math.floor((lon + 180.0) / s), math.floor((lat + 90.0) / s)


def _ground_points(parts) -> np.ndarray:
    pts = [np.asarray(p.exterior.coords) for p in parts]
    pts.append(np.asarray([shapely.MultiPolygon(parts).centroid.coords[0]]))
    return np.concatenate(pts)


def plan_buildings(footprints, heights_fn, min_area_m2: float) -> tuple[list[Building], dict]:
    stats = {"input": len(footprints), "kept": 0, "dropped_small": 0, "dropped_invalid": 0, "dropped_no_height": 0}
    staged = []
    for fp in footprints:
        if (
            fp.geometry is None
            or fp.geometry.is_empty
            or fp.geometry.geom_type not in ("Polygon", "MultiPolygon", "GeometryCollection")
        ):
            stats["dropped_invalid"] += 1
            continue
        parts = clean(fp.geometry, min_area_m2)
        if not parts:
            stats["dropped_small"] += 1
            continue
        c = shapely.MultiPolygon(parts).centroid
        area = sum(_area_m2(p, c.y) for p in parts)
        staged.append((fp, parts, c.x, c.y, area))
    groups: dict[tuple[int, int], list] = {}
    for item in staged:
        groups.setdefault(tile_of(item[2], item[3], LEAF_ZOOM), []).append(item)
    out = []
    for key in sorted(groups):
        items = groups[key]
        pts = [_ground_points(it[1]) for it in items]
        allp = np.concatenate(pts)
        h = np.asarray(heights_fn(allp[:, 0], allp[:, 1]), np.float64)
        i = 0
        for (fp, parts, cx, cy, area), p in zip(items, pts):
            g = h[i : i + len(p)]
            i += len(p)
            if not np.isfinite(g).all():
                stats["dropped_no_height"] += 1
                continue
            height, src = building_height(fp.height, fp.num_floors)
            base = float(g.min()) - SINK_M
            z = tier_zoom(area)
            out.append(
                Building(
                    fp.source_id,
                    parts,
                    fp.cls,
                    height,
                    src,
                    base,
                    base + SINK_M + height,
                    area,
                    (z, *tile_of(cx, cy, z)),
                )
            )
    out.sort(key=lambda b: b.source_id)
    stats["kept"] = len(out)
    return out, stats


def _cdt_triangles(poly) -> list[np.ndarray]:
    """Roof triangles (3 x 2 lon/lat each, CCW), triangulated in a locally isotropic plane."""
    sx, sy = local_scale(poly.centroid.y)
    scaled = shapely.affinity.scale(poly, xfact=sx / sy, yfact=1.0, origin=(0, 0))
    tris = []
    for t in shapely.get_parts(shapely.constrained_delaunay_triangles(scaled)):
        c = np.asarray(t.exterior.coords)[:3]
        c[:, 0] *= sy / sx
        a = (c[1, 0] - c[0, 0]) * (c[2, 1] - c[0, 1]) - (c[2, 0] - c[0, 0]) * (c[1, 1] - c[0, 1])
        if a == 0:
            continue
        tris.append(c if a > 0 else c[::-1])
    return tris


def tile_geometry(buildings: list[Building], origin: tuple[float, float, float]) -> TileGeometry:
    m = geodesy.enu_to_ecef_matrix(*origin)
    rot, o = m[:3, :3].T, m[:3, 3]  # ECEF -> ENU rotation, origin

    def to_gl(lonlat: np.ndarray, h: float) -> np.ndarray:
        e = geodesy.geodetic_to_ecef(lonlat[:, 0], lonlat[:, 1], np.full(len(lonlat), h))
        enu = (rot @ (e - o).T).T
        return np.stack([enu[:, 0], enu[:, 2], -enu[:, 1]], axis=1)

    pos, fid, wall = [], [], []
    roof: dict[int, list[int]] = {}
    n = 0

    def add(tri_gl: np.ndarray, out: list, f: int):
        nonlocal n
        pos.append(tri_gl)
        fid.extend([f] * len(tri_gl))
        out.extend(range(n, n + len(tri_gl)))
        n += len(tri_gl)

    bs = sorted(buildings, key=lambda b: b.source_id)
    for f, b in enumerate(bs):
        for part in b.parts:
            for tri in _cdt_triangles(part):
                add(to_gl(tri, b.roof_m), roof.setdefault(f, []), f)
            for ring in [part.exterior, *part.interiors]:
                c = np.asarray(ring.coords)
                bot, top = to_gl(c, b.base_m), to_gl(c, b.roof_m)
                for k in range(len(c) - 1):
                    if np.linalg.norm(bot[k + 1] - bot[k]) < MIN_EDGE_M:
                        continue
                    a0, b0, b1, a1 = bot[k], bot[k + 1], top[k + 1], top[k]
                    add(np.stack([a0, b0, b1, a0, b1, a1]), wall, f)
    p = np.concatenate(pos) if pos else np.zeros((0, 3))
    tri = p.reshape(-1, 3, 3)
    nrm = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    nrm /= np.linalg.norm(nrm, axis=1, keepdims=True)
    table = PropertyTable(
        [b.source_id for b in bs],
        [b.cls for b in bs],
        np.asarray([b.height_m for b in bs], np.float32),
        np.asarray([b.height_source for b in bs], np.uint8),
    )
    return TileGeometry(
        p.astype(np.float32),
        np.repeat(nrm, 3, axis=0).astype(np.float32),
        np.asarray(fid, np.float32),
        {f: np.asarray(v, np.uint32) for f, v in roof.items()},
        np.asarray(wall, np.uint32),
        table,
        m,
    )


def tile_mesh(
    buildings: list[Building],
    origin: tuple[float, float, float],
    roof: RoofOptions | None = None,
    sampler: RoofSampler | None = None,
    stats: dict | None = None,
) -> tuple[TileMesh, PropertyTable]:
    """The tile's mesh. Without `roof` and `sampler` it has R3's layout (flat roofs, no texcoords, no extras); with
    them each roof is drawn from the tile's atlas or, when roofs.roof_texture leaves it out, kept flat. `stats`
    accumulates textured / flat_coarse / flat_overflow roof counts and tiles_by_texel_m."""
    g = tile_geometry(buildings, origin)

    def cat(fs) -> np.ndarray:
        return np.concatenate([g.roof[f] for f in fs]).astype(np.uint32) if fs else np.zeros(0, np.uint32)

    if roof is None or sampler is None:
        return TileMesh(g.positions, g.normals, g.feature_ids, cat(sorted(g.roof)), g.wall_indices), g.table
    tex = roof_texture(g.positions, g.roof, g.table.source_ids, g.matrix, sampler, roof)
    textured, flat = sorted(tex.textured), sorted(set(g.roof) - tex.textured)
    if stats is not None:
        for k, v in (("textured", len(textured)), ("flat_coarse", tex.n_coarse), ("flat_overflow", tex.n_overflow)):
            stats[k] = stats.get(k, 0) + v
        by = stats.setdefault("tiles_by_texel_m", {})
        if tex.texel_m is not None:
            by[f"{tex.texel_m:g}"] = by.get(f"{tex.texel_m:g}", 0) + 1
    mesh = TileMesh(
        g.positions, g.normals, g.feature_ids, cat(flat), g.wall_indices,
        textured_indices=cat(textured), texcoords=tex.texcoords if textured else None, texture_jpeg=tex.jpeg,
        extras={"roof_texel_m": tex.texel_m},
    )
    return mesh, g.table


def tile_origin(z: int, x: int, y: int, blds: list[Building]) -> tuple[float, float, float]:
    w, s, e, n = tile_bounds(z, x, y)
    lo = min(b.base_m for b in blds)
    hi = max(b.roof_m for b in blds)
    return (w + e) / 2.0, (s + n) / 2.0, round((lo + hi) / 2.0, 3)


def _region(blds: list[Building]) -> list[float]:
    w = min(p.bounds[0] for b in blds for p in b.parts)
    s = min(p.bounds[1] for b in blds for p in b.parts)
    e = max(p.bounds[2] for b in blds for p in b.parts)
    n = max(p.bounds[3] for b in blds for p in b.parts)
    lo = min(b.base_m for b in blds)
    hi = max(b.roof_m for b in blds)
    return [math.radians(w), math.radians(s), math.radians(e), math.radians(n), lo, hi]


def _union(regions: list[list[float]]) -> list[float]:
    r = np.asarray(regions)
    return [
        float(r[:, 0].min()),
        float(r[:, 1].min()),
        float(r[:, 2].max()),
        float(r[:, 3].max()),
        float(r[:, 4].min()),
        float(r[:, 5].max()),
    ]


def _rigid_inverse(m: np.ndarray) -> np.ndarray:
    """Exact inverse of a rotation + translation matrix: [R^T | -R^T t] (no LAPACK, so byte-identical everywhere)."""
    out = np.eye(4)
    out[:3, :3] = m[:3, :3].T
    out[:3, 3] = -(m[:3, :3].T @ m[:3, 3])
    return out


def tileset_json(tiles: dict) -> dict:
    """tiles: (z, x, y) -> (buildings, ENU origin). Every ancestor of a content tile down from z13 exists in the tree
    (content-less when it holds no building of its own tier)."""
    from . import __version__

    keys = set(tiles)
    nodes = set()
    for z, x, y in keys:
        while z >= min(TIER_MIN_AREA_M2):
            nodes.add((z, x, y))
            z, x, y = z - 1, x // 2, y // 2

    def children(z, x, y):
        return sorted(k for k in nodes if k[0] == z + 1 and (k[1] // 2, k[2] // 2) == (x, y))

    def node(key, parent_abs: np.ndarray) -> dict:
        z, x, y = key
        kids_abs = parent_abs
        out: dict = {"geometricError": GEOMETRIC_ERROR[z]}
        regions = []
        if key in tiles:
            blds, origin = tiles[key]
            own = geodesy.enu_to_ecef_matrix(*origin)
            out["transform"] = geodesy.matrix_to_column_major(_rigid_inverse(parent_abs) @ own)
            out["content"] = {"uri": f"{z}/{x}/{y}.glb"}
            kids_abs = own
            regions.append(_region(blds))
        kids = [node(k, kids_abs) for k in children(z, x, y)]
        regions += [k["boundingVolume"]["region"] for k in kids]
        out["boundingVolume"] = {"region": _union(regions)}
        if kids:
            out["children"] = kids
        return out

    top = [node(k, np.eye(4)) for k in sorted(k for k in nodes if k[0] == min(TIER_MIN_AREA_M2))]
    root = {
        "boundingVolume": {"region": _union([t["boundingVolume"]["region"] for t in top])},
        "geometricError": GEOMETRIC_ERROR["root"],
        "refine": "ADD",
        "children": top,
    }
    return {
        "asset": {"version": "1.1", "generator": f"camsim-scene {__version__}"},
        "geometricError": GEOMETRIC_ERROR["root"],
        "root": root,
    }


def walk_tileset(doc: dict) -> list[tuple[str, np.ndarray]]:
    out = []

    def visit(n: dict, parent: np.ndarray):
        m = parent @ geodesy.column_major_to_matrix(n["transform"]) if "transform" in n else parent
        if "content" in n:
            out.append((n["content"]["uri"], m))
        for c in n.get("children", []):
            visit(c, m)

    visit(doc["root"], np.eye(4))
    return out
