"""Bridge decks (REALISM R3, spec docs/superpowers/specs/2026-10-15-bridges-ripples-design.md section 1): OSM bridge
ways -> deck heights from the bare-earth terrain at their grounded ends -> a swept deck cross-section with parapets and
piers -> 3D Tiles 1.1 under features/bridges/."""

from __future__ import annotations

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
TILE_ZOOM = 13
GEOMETRIC_ERROR = 60.0


import math
from dataclasses import dataclass

import numpy as np
from shapely.geometry import LineString

from .features import Feature, width_m

M_PER_DEG = 111320.0
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


def _scale(lat: float) -> tuple[float, float]:
    return M_PER_DEG * math.cos(math.radians(lat)), M_PER_DEG


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
