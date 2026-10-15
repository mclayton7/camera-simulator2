"""Bridge decks (REALISM R3, spec docs/superpowers/specs/2026-10-15-bridges-ripples-design.md section 1): OSM bridge
ways -> deck heights from the bare-earth terrain at their grounded ends -> a swept deck cross-section with parapets and
piers -> 3D Tiles 1.1 under features/bridges/."""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np
import shapely
from shapely.geometry import LineString, Polygon

from . import geodesy, gltf
from .features import Feature, width_m

BRIDGES_RULES_VERSION = "1"  # bump on any change to selection, heights or geometry (it is in the layer settings hash)
MIN_CLEARANCE_M = 2.0  # a deck never higher than this above the terrain is not built (the terrain carries the road)
DECK_THICKNESS_M = 1.5
PARAPET_W_M, PARAPET_H_M = 0.3, 1.0
PIER_SPACING_M = 30.0
PIER_MIN_CLEARANCE_M = 3.0
PIER_END_MARGIN_M = 10.0
PIER_ALONG_M = 1.2
PIER_ACROSS_FRACTION = 0.6
PIER_SINK_M = 0.5
STATION_MAX_M = 10.0
PROFILE_STEP_M = 5.0
TILE_ZOOM = 13  # content tiles, by deck midpoint
GROUP_ZOOM = 10  # content-less groups (~35 km at 33 N): a group's decks load when the camera is within ~10 km of it
GROUP_GEOMETRIC_ERROR = 700.0  # 700 * 1080 / (2 tan 30 deg * 64) ~ 10 km at SSE 64, 1080p, 60 deg FOV
ROOT_GEOMETRIC_ERROR = 100000.0  # the root always refines: the groups decide


NODE_DIGITS = 9  # OSM shared nodes give identical coordinates; 1e-9 deg is ~0.1 mm


@dataclass(frozen=True)
class Deck:
    source_id: str
    kind: str  # "road" | "rail"
    line: LineString  # lon/lat, in the OSM way's direction
    width_m: float
    h0: float  # deck top at the first point, WGS-84 ellipsoid m
    h1: float  # deck top at the last point
    length_m: float
    max_clearance_m: float


def _skip(t: dict) -> bool:
    if t.get("bridge", "no") == "no" or t.get("tunnel", "no") != "no":
        return True
    if t.get("covered") == "yes" or t.get("location") == "underground":
        return True
    try:
        return float(t.get("layer", "0")) < 0
    except ValueError:
        return False


def select(features: list[Feature]) -> list[Feature]:
    """The bridge ways among R2's OSM features: roads and rail tagged bridge, not tunnels, covered or below ground."""
    out = [f for f in features if f.kind in ("road", "rail") and f.geom.geom_type == "LineString" and not _skip(f.tags)]
    return sorted(out, key=lambda f: f.source_id)


WGS84_A, WGS84_E2 = 6378137.0, 6.69437999014e-3


def _scale(lat: float) -> tuple[float, float]:
    """Metres per degree of longitude and latitude at `lat` on the WGS-84 ellipsoid (prime-vertical and meridian
    radii): exact to first order, so a deck's width and parapets come out at their metric size."""
    s = math.sin(math.radians(lat))
    w = 1.0 - WGS84_E2 * s * s
    n = WGS84_A / math.sqrt(w)
    m = WGS84_A * (1.0 - WGS84_E2) / w**1.5
    return math.radians(1.0) * n * math.cos(math.radians(lat)), math.radians(1.0) * m


def metric(line: LineString, lat0: float, lon0: float) -> np.ndarray:
    """(n, 2) metres east / north of (lon0, lat0), equirectangular (bridges are < 1 km: the error is millimetres)."""
    sx, sy = _scale(lat0)
    c = np.asarray(line.coords, np.float64)[:, :2]
    return np.stack([(c[:, 0] - lon0) * sx, (c[:, 1] - lat0) * sy], axis=1)


def length_m(line: LineString) -> float:
    c = line.coords[0]
    p = metric(line, c[1], c[0])
    return float(np.linalg.norm(np.diff(p, axis=0), axis=1).sum())


def deck_height(d: Deck, s_m: float) -> float:
    """Deck top at arc length s_m from the first point: linear between the two end nodes."""
    if d.length_m <= 0:
        return d.h0
    t = min(1.0, max(0.0, s_m / d.length_m))
    return d.h0 + (d.h1 - d.h0) * t


def _key(pt) -> tuple:
    return (round(pt[0], NODE_DIGITS), round(pt[1], NODE_DIGITS))


def _samples(line: LineString, step_m: float) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Points every <= step_m along the line (ends included): lon, lat, arc length (m)."""
    c0 = line.coords[0]
    p = metric(line, c0[1], c0[0])
    seg = np.linalg.norm(np.diff(p, axis=0), axis=1)
    cum = np.concatenate([[0.0], np.cumsum(seg)])
    total = cum[-1]
    n = max(2, int(math.ceil(total / step_m)) + 1)
    s = np.linspace(0.0, total, n)
    c = np.asarray(line.coords, np.float64)[:, :2]
    lon = np.interp(s, cum, c[:, 0]) if total > 0 else np.full(n, c[0, 0])
    lat = np.interp(s, cum, c[:, 1]) if total > 0 else np.full(n, c[0, 1])
    return lon, lat, s


def local_heights(heights_fn, cell_deg: float = 0.05):
    """heights_fn, called once per cell_deg x cell_deg cell of the points instead of once for all of them: the
    terrain sampler opens every source tile under a call's bounding box, so one call over a whole package's bridge ends
    would read the terrain of the whole box (1.6 GB at Pendleton)."""

    def fn(lon, lat):
        lon, lat = np.asarray(lon, np.float64), np.asarray(lat, np.float64)
        out = np.full(lon.shape, np.nan)
        cells = np.stack([np.floor(lon / cell_deg), np.floor(lat / cell_deg)], axis=-1).reshape(-1, 2)
        flat_lon, flat_lat, flat_out = lon.reshape(-1), lat.reshape(-1), out.reshape(-1)
        for c in np.unique(cells, axis=0):
            m = (cells == c).all(axis=1)
            flat_out[m] = np.asarray(heights_fn(flat_lon[m], flat_lat[m]), np.float64)
        return out

    return fn


def plan_decks(features: list[Feature], heights_fn, min_clearance_m: float = MIN_CLEARANCE_M) -> tuple[list[Deck], dict]:
    """Deck heights over the bridge graph (spec 1.2): per kind, way end points are nodes; degree-1 nodes are grounded
    at the bare-earth terrain (heights_fn: lon, lat arrays -> ellipsoid m, NaN = no data); every free node takes the
    1/length-weighted mean of its neighbours (one linear solve per connected component). Height is linear in arc
    length along a way. Ways never more than min_clearance_m above the terrain are dropped (the terrain carries them)."""
    ways = select(features)
    stats = {"input": len(ways), "kept": 0, "dropped_low": 0, "dropped_ungrounded": 0, "dropped_no_height": 0}
    if not ways:
        return [], stats
    lengths = {w.source_id: length_m(w.geom) for w in ways}
    ends = {w.source_id: ((w.kind, _key(w.geom.coords[0])), (w.kind, _key(w.geom.coords[-1]))) for w in ways}
    degree: dict = {}
    for a, b in ends.values():
        degree[a] = degree.get(a, 0) + 1
        degree[b] = degree.get(b, 0) + 1
    grounded = sorted(k for k, n in degree.items() if n == 1)
    gh = {}
    if grounded:
        h = np.asarray(heights_fn(np.array([k[1][0] for k in grounded]), np.array([k[1][1] for k in grounded])), float)
        gh = dict(zip(grounded, h.tolist()))
    bad = {k for k, v in gh.items() if not math.isfinite(v)}
    live = []
    for w in ways:
        if any(e in bad for e in ends[w.source_id]):
            stats["dropped_no_height"] += 1
        else:
            live.append(w)
    # connected components over the live ways (union-find on nodes)
    parent: dict = {}

    def find(x):
        while parent.setdefault(x, x) != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for w in live:
        a, b = ends[w.source_id]
        parent[find(a)] = find(b)
    comps: dict = {}
    for w in live:
        comps.setdefault(find(ends[w.source_id][0]), []).append(w)
    node_h: dict = {}
    solved = []
    for root in sorted(comps, key=lambda r: min(w.source_id for w in comps[r])):
        cw = comps[root]
        nodes = sorted({e for w in cw for e in ends[w.source_id]})
        if not any(n in gh for n in nodes):
            stats["dropped_ungrounded"] += len(cw)
            continue
        free = [n for n in nodes if n not in gh]
        idx = {n: i for i, n in enumerate(free)}
        if free:
            A = np.zeros((len(free), len(free)))
            rhs = np.zeros(len(free))
            for w in cw:
                a, b = ends[w.source_id]
                wt = 1.0 / max(lengths[w.source_id], 1e-3)
                for u, v in ((a, b), (b, a)):
                    if u not in idx:
                        continue
                    A[idx[u], idx[u]] += wt
                    if v in idx:
                        A[idx[u], idx[v]] -= wt
                    else:
                        rhs[idx[u]] += wt * gh[v]
            sol = np.linalg.solve(A, rhs)
            node_h.update({n: float(sol[i]) for n, i in idx.items()})
        node_h.update({n: gh[n] for n in nodes if n in gh})
        solved += cw
    out = []
    for w in sorted(solved, key=lambda w: w.source_id):
        a, b = ends[w.source_id]
        d = Deck(w.source_id, w.kind, w.geom, width_m(w), node_h[a], node_h[b], lengths[w.source_id], 0.0)
        lon, lat, s = _samples(w.geom, PROFILE_STEP_M)
        terr = np.asarray(heights_fn(lon, lat), float)
        top = np.array([deck_height(d, x) for x in s])
        clear = top - terr
        mc = float(np.nanmax(clear)) if np.isfinite(clear).any() else 0.0
        if mc < min_clearance_m:
            stats["dropped_low"] += 1
            continue
        out.append(Deck(d.source_id, d.kind, d.line, d.width_m, d.h0, d.h1, d.length_m, mc))
    stats["kept"] = len(out)
    return out, stats


# --- geometry (spec 1.3) ---------------------------------------------------------------------------------------------


MIN_STEP_M = 0.05  # consecutive points closer than this are one station
MAX_MITRE = 2.0
ASPHALT_RGBA = (0.20, 0.20, 0.21, 1.0)
CONCRETE_RGBA = (0.55, 0.54, 0.52, 1.0)
BALLAST_RGBA = (0.36, 0.33, 0.30, 1.0)
SCHEMA = {
    "id": "camsim_bridges",
    "classes": {
        "bridge": {
            "properties": {
                "kind": {"type": "STRING"},
                "max_clearance_m": {"type": "SCALAR", "componentType": "FLOAT32"},
                "source_id": {"type": "STRING"},
            }
        }
    },
}


def section(width_m: float) -> np.ndarray:
    """The deck cross-section (across, up) relative to the deck top at the centreline, counter-clockwise; edge 4 -> 5
    is the driving surface."""
    hw, pw, ph, t = width_m / 2.0, PARAPET_W_M, PARAPET_H_M, DECK_THICKNESS_M
    return np.array(
        [(-hw - pw, -t), (hw + pw, -t), (hw + pw, ph), (hw, ph), (hw, 0.0), (-hw, 0.0), (-hw, ph), (-hw - pw, ph)]
    )


DRIVING_EDGE = 4


def stations(d: Deck) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Densified centreline in metres east / north of the deck's first point, arc length, unit left offset vectors
    (mitred, capped at MAX_MITRE). Points closer than MIN_STEP_M are merged; segments longer than STATION_MAX_M split."""
    c0 = d.line.coords[0]
    p = metric(d.line, c0[1], c0[0])
    keep = [p[0]]
    for q in p[1:]:
        if np.linalg.norm(q - keep[-1]) >= MIN_STEP_M:
            keep.append(q)
    p = np.asarray(keep)
    if len(p) < 2:
        return np.zeros((0, 2)), np.zeros(0), np.zeros((0, 2))
    pts = [p[0]]
    for a, b in zip(p[:-1], p[1:]):
        n = max(1, int(math.ceil(np.linalg.norm(b - a) / STATION_MAX_M)))
        pts += [a + (b - a) * (k / n) for k in range(1, n + 1)]
    pts = np.asarray(pts)
    seg = np.diff(pts, axis=0)
    seg_len = np.linalg.norm(seg, axis=1)
    tang = seg / seg_len[:, None]
    nrm = np.stack([-tang[:, 1], tang[:, 0]], axis=1)  # left of travel
    off = np.zeros_like(pts)
    off[0], off[-1] = nrm[0], nrm[-1]
    for i in range(1, len(pts) - 1):
        m = nrm[i - 1] + nrm[i]
        if np.linalg.norm(m) < 1e-6:  # a hairpin: no bisector
            off[i] = nrm[i]
            continue
        m /= np.linalg.norm(m)
        off[i] = m * min(MAX_MITRE, 1.0 / max(float(np.dot(m, nrm[i])), 1.0 / MAX_MITRE))
    s = np.concatenate([[0.0], np.cumsum(seg_len)])
    return pts, s, off


def pier_positions(length: float) -> list[float]:
    """Arc lengths of the pier candidates: evenly PIER_SPACING_M apart, centred, at least PIER_END_MARGIN_M from ends."""
    span = length - 2 * PIER_END_MARGIN_M
    if span < 0:
        return []
    n = int(math.floor(span / PIER_SPACING_M + 1e-9)) + 1
    start = (length - (n - 1) * PIER_SPACING_M) / 2.0
    return [start + k * PIER_SPACING_M for k in range(n)]


class _Builder:
    def __init__(self):
        self.pos: list = []
        self.nrm: list = []
        self.fid: list = []
        self.top: list = []
        self.rest: list = []

    def tri(self, a, b, c, out_dir, f: int, top: bool):
        """One triangle (3-vectors east, north, up), wound so its normal agrees with out_dir."""
        n = np.cross(b - a, c - a)
        ln = np.linalg.norm(n)
        if ln < 1e-9:
            return
        n /= ln
        if np.dot(n, out_dir) < 0:
            b, c, n = c, b, -n
        k = len(self.pos)
        self.pos += [a, b, c]
        self.nrm += [n, n, n]
        self.fid += [f, f, f]
        (self.top if top else self.rest).extend([k, k + 1, k + 2])

    def quad(self, a, b, c, d, out_dir, f, top=False):
        self.tri(a, b, c, out_dir, f, top)
        self.tri(a, c, d, out_dir, f, top)


def _sweep(b: _Builder, d: Deck, f: int, heights_fn) -> None:
    pts, s, off = stations(d)
    if len(pts) < 2:
        return
    sec = section(d.width_m)
    h = np.array([deck_height(d, x) for x in s])
    up = np.array([0.0, 0.0, 1.0])

    def P(i, j):
        a, u = sec[j]
        return np.array([pts[i, 0] + a * off[i, 0], pts[i, 1] + a * off[i, 1], h[i] + u])

    nsec = len(sec)
    for j in range(nsec):
        j2 = (j + 1) % nsec
        e = sec[j2] - sec[j]
        na, nu = e[1], -e[0]  # outward normal of a counter-clockwise polygon's edge, (across, up)
        for i in range(len(pts) - 1):
            lft = off[i] / np.linalg.norm(off[i])
            out = np.array([lft[0] * na, lft[1] * na, nu])
            b.quad(P(i, j), P(i, j2), P(i + 1, j2), P(i + 1, j), out, f, top=(j == DRIVING_EDGE))
    tris = [np.asarray(t.exterior.coords)[:3] for t in shapely.get_parts(shapely.constrained_delaunay_triangles(Polygon(sec)))]
    for i, sign in ((0, -1.0), (len(pts) - 1, 1.0)):
        k = min(i, len(pts) - 2)
        t2 = pts[k + 1] - pts[k]
        t2 = t2 / np.linalg.norm(t2)
        lft = off[i] / np.linalg.norm(off[i])
        out = np.array([t2[0], t2[1], 0.0]) * sign
        for tri in tris:
            v = [np.array([pts[i, 0] + a * lft[0], pts[i, 1] + a * lft[1], h[i] + u]) for a, u in tri]
            b.tri(v[0], v[1], v[2], out, f, False)
    # piers (spec 1.3)
    cand = pier_positions(float(s[-1]))
    if not cand:
        return
    xy = np.array([[np.interp(x, s, pts[:, 0]), np.interp(x, s, pts[:, 1])] for x in cand])
    c0 = d.line.coords[0]
    sx, sy = _scale(c0[1])
    terr = np.asarray(heights_fn(c0[0] + xy[:, 0] / sx, c0[1] + xy[:, 1] / sy), float)
    for x, (px, py), tz in zip(cand, xy, terr):
        bottom_of_deck = deck_height(d, x) - DECK_THICKNESS_M
        if not math.isfinite(tz) or bottom_of_deck - tz < PIER_MIN_CLEARANCE_M:
            continue
        k = min(int(np.searchsorted(s, x, side="right")) - 1, len(pts) - 2)
        t2 = pts[k + 1] - pts[k]
        t2 = t2 / np.linalg.norm(t2)
        lf = np.array([-t2[1], t2[0]])
        ha, hc = PIER_ALONG_M / 2.0, PIER_ACROSS_FRACTION * d.width_m / 2.0
        z0, z1 = tz - PIER_SINK_M, bottom_of_deck
        corners = [np.array([px, py]) + sa * ha * t2 + sc * hc * lf for sa, sc in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        for q in range(4):
            a2, b2 = corners[q], corners[(q + 1) % 4]
            mid = (a2 + b2) / 2 - np.array([px, py])
            out = np.array([mid[0], mid[1], 0.0])
            b.quad(np.r_[a2, z0], np.r_[b2, z0], np.r_[b2, z1], np.r_[a2, z1], out, f)
        b.quad(*[np.r_[c, z0] for c in corners], -up, f)


def deck_mesh(decks: list[Deck], origin: tuple[float, float, float], heights_fn) -> gltf.TileMesh:
    """The decks' mesh in the tile's ENU frame (glTF axes east, up, -north). roof_indices = driving surfaces,
    wall_indices = everything else; feature id = index in the decks sorted by source_id."""
    ds = sorted(decks, key=lambda d: d.source_id)
    m = geodesy.enu_to_ecef_matrix(*origin)
    rot, o = m[:3, :3].T, m[:3, 3]
    pos, nrm, fid, top, rest = [], [], [], [], []
    k = 0  # vertices so far
    for f, d in enumerate(ds):
        b = _Builder()
        _sweep(b, d, f, heights_fn)
        if not b.pos:
            continue
        p = np.asarray(b.pos)  # metres east / north of the deck's first point, ellipsoid height
        c0 = d.line.coords[0]
        sx, sy = _scale(c0[1])
        e = geodesy.geodetic_to_ecef(c0[0] + p[:, 0] / sx, c0[1] + p[:, 1] / sy, p[:, 2])
        enu = (rot @ (e - o).T).T
        # normals: local east/north/up of the deck start to the tile ENU (bridges are small: the rotation is ~1e-5 rad)
        ne = np.asarray(b.nrm)
        pos.append(np.stack([enu[:, 0], enu[:, 2], -enu[:, 1]], axis=1))
        nrm.append(np.stack([ne[:, 0], ne[:, 2], -ne[:, 1]], axis=1))
        fid.append(np.asarray(b.fid, np.float32))
        top.append(np.asarray(b.top, np.int64) + k)
        rest.append(np.asarray(b.rest, np.int64) + k)
        k += len(p)
    if not pos:
        z = np.zeros((0, 3), np.float32)
        return gltf.TileMesh(z, z, np.zeros(0, np.float32), np.zeros(0, np.uint32), np.zeros(0, np.uint32))
    return gltf.TileMesh(
        np.concatenate(pos).astype(np.float32),
        np.concatenate(nrm).astype(np.float32),
        np.concatenate(fid),
        np.concatenate(top).astype(np.uint32),
        np.concatenate(rest).astype(np.uint32),
    )


def bridges_glb(decks: list[Deck], origin: tuple[float, float, float], heights_fn, copyright: str) -> bytes:
    """One tile's GLB: material 0 road driving surface (asphalt), 1 the rest (concrete), 2 rail driving surface
    (ballast); EXT_structural_metadata class `bridge` (source_id, kind, max_clearance_m)."""
    ds = sorted(decks, key=lambda d: d.source_id)
    mesh = deck_mesh(ds, origin, heights_fn)
    rail = np.array([d.kind == "rail" for d in ds])
    fid_top = mesh.feature_ids[mesh.roof_indices].astype(int) if len(mesh.roof_indices) else np.zeros(0, int)
    is_rail = rail[fid_top] if len(fid_top) else np.zeros(0, bool)
    groups = [
        (0, mesh.roof_indices[~is_rail]),
        (1, mesh.wall_indices),
        (2, mesh.roof_indices[is_rail]),
    ]
    columns = {
        "kind": ("string", [d.kind for d in ds]),
        "max_clearance_m": ("float32", np.asarray([d.max_clearance_m for d in ds], np.float32)),
        "source_id": ("string", [d.source_id for d in ds]),
    }
    return gltf.write_feature_glb(
        mesh.positions, mesh.normals, mesh.feature_ids, groups, [ASPHALT_RGBA, CONCRETE_RGBA, BALLAST_RGBA],
        SCHEMA, "bridge", columns, copyright,
    )
