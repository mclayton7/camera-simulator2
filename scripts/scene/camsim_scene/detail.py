"""Detail polygons (REALISM R5, docs/superpowers/specs/2026-10-14-r5-detail-polygons-design.md): named areas built
at the detail profile (sim) inside a package built at its own profile (preview).

Polygons come from a GeoJSON FeatureCollection, are validated and normalised once by `plan` and live on in the
manifest (`detail`) and as regions (`detail:<name>` at the detail zooms, `detail_margin:<name>` = the polygon grown by
imagery_margin_km); builds never read the file again. Sources only the detail profile uses are *scoped*: discovered
per polygon, their footprints clipped to the scope, their samples masked to it."""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np
import shapely
import shapely.affinity
import shapely.geometry

from .config import KM_PER_DEG, NAME_RE, ConfigError
from .fsutil import sha256_bytes
from .manifest import canonical_json
from .tiling import Bounds, Region

DETAIL = "detail"
MARGIN = "detail_margin"
KINDS = (DETAIL, MARGIN)
PRECISION = 1e-7
WGS84_NAMES = ("EPSG::4326", "EPSG:4326", "CRS84")


def normalise(geom):
    return shapely.normalize(shapely.set_precision(geom, PRECISION))


def load_polygons(path: Path, bbox: Bounds) -> list[dict]:
    """The file's features as [{"name", "wkt"}] sorted by name; ConfigError naming the feature on any problem."""
    try:
        doc = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise ConfigError(f"detail.file {path}: {e}") from e
    feats = doc.get("features") if isinstance(doc, dict) else None
    if not isinstance(doc, dict) or doc.get("type") != "FeatureCollection" or not isinstance(feats, list) or not feats:
        raise ConfigError(f"detail.file {path}: needs a GeoJSON FeatureCollection with at least one feature")
    crs = doc.get("crs")
    if crs is not None:
        name = str(((crs or {}).get("properties") or {}).get("name", "")) if isinstance(crs, dict) else ""
        if not name.endswith(WGS84_NAMES):
            raise ConfigError(f"detail.file {path}: crs must be EPSG:4326 / CRS84 lon/lat, got {name!r}")
    box = shapely.box(*bbox)
    out, seen = [], set()
    for i, f in enumerate(feats):
        name = ((f or {}).get("properties") or {}).get("name") if isinstance(f, dict) else None
        where = f"detail.file feature {i} ({name!r})"
        if not isinstance(name, str) or not NAME_RE.match(name):
            raise ConfigError(f"{where}: properties.name must match {NAME_RE.pattern}")
        if name in seen:
            raise ConfigError(f"{where}: duplicate name")
        seen.add(name)
        try:
            geom = shapely.geometry.shape(f.get("geometry"))
        except Exception as e:  # any malformed geometry is a config error
            raise ConfigError(f"{where}: bad geometry: {e}") from e
        if geom.geom_type not in ("Polygon", "MultiPolygon"):
            raise ConfigError(f"{where}: geometry must be a Polygon or MultiPolygon, got {geom.geom_type}")
        if geom.is_empty or not geom.is_valid:
            raise ConfigError(f"{where}: geometry is empty or invalid ({shapely.is_valid_reason(geom)})")
        geom = normalise(geom)
        if geom.is_empty or not geom.is_valid:
            raise ConfigError(f"{where}: geometry is invalid after snapping to 1e-7 degrees")
        if not box.covers(geom):  # inside the bbox, so it cannot cross the antimeridian either
            raise ConfigError(f"{where}: must lie inside the bbox {list(bbox)}")
        out.append({"name": name, "wkt": shapely.to_wkt(geom, rounding_precision=7)})
    return sorted(out, key=lambda p: p["name"])


def polygons_sha256(polygons: list[dict]) -> str:
    return sha256_bytes(canonical_json(polygons).encode())


def section(polygons: list[dict], profile: str) -> dict:
    """The manifest's `detail` section."""
    return {"profile": profile, "polygons": polygons, "sha256": polygons_sha256(polygons)}


def buffer_km(geom, km: float):
    """geom grown by km: degrees of latitude, longitude scaled by the pole-ward latitude (as config.ring_bounds, so
    the pad is never too small)."""
    if km <= 0:
        return geom
    _, s, _, n = geom.bounds
    dlat = km / KM_PER_DEG
    k = math.cos(math.radians(min(89.0, max(abs(s), abs(n)) + dlat)))
    g = shapely.affinity.scale(geom, xfact=k, yfact=1.0, origin=(0.0, 0.0))
    g = g.buffer(dlat, quad_segs=8)
    return normalise(shapely.affinity.scale(g, xfact=1.0 / k, yfact=1.0, origin=(0.0, 0.0)))


def regions(polygons: list[dict], detail_zoom: dict, margin_zoom: dict, margin_km: float) -> list[Region]:
    """`detail:<name>[/<part>]` per polygon part at detail_zoom, then `detail_margin:<name>[/<part>]` (the part grown by
    margin_km) at margin_zoom."""
    out: list[Region] = []
    for kind, zoom, grow in ((DETAIL, detail_zoom, 0.0), (MARGIN, margin_zoom, margin_km)):
        for p in polygons:
            parts = shapely.get_parts(shapely.from_wkt(p["wkt"])).tolist()
            for i, part in enumerate(parts):
                suffix = f"/{i}" if len(parts) > 1 else ""
                out.append(Region.from_polygon(f"{kind}:{p['name']}{suffix}", buffer_km(part, grow), zoom))
    return out


def scope_geometry(regions_: list[Region], kind: str):
    """Union of the regions of one kind ("detail" or "detail_margin"), or None when there are none."""
    gs = [r.geometry() for r in regions_ if r.name.split(":", 1)[0] == kind]
    return shapely.union_all(gs) if gs else None


def scoped_map(layers: dict) -> dict[str, str]:
    """Source id -> scope kind, from the manifest's (or layer_settings') terrain and imagery `scoped` tables."""
    out: dict[str, str] = {}
    for layer in ("terrain", "imagery"):
        out.update((layers.get(layer) or {}).get("scoped", {}))
    return out


def scopes(m) -> dict:
    """Source id -> scope geometry for a Manifest (empty without [detail])."""
    kinds = scoped_map(m.layers)
    if not kinds:
        return {}
    rs = m.region_objs()
    geoms = {k: scope_geometry(rs, k) for k in set(kinds.values())}
    return {sid: geoms[k] for sid, k in kinds.items() if geoms[k] is not None}


def clip(geom, scope):
    """The polygonal part of geom inside scope, snapped to 1e-7 degrees; None when nothing with area is left."""
    g = shapely.intersection(geom, scope)
    parts = [p for p in shapely.get_parts(g) if p.geom_type in ("Polygon", "MultiPolygon") and p.area > 0]
    if not parts:
        return None
    g = shapely.set_precision(shapely.union_all(parts), PRECISION)
    return None if g.is_empty else g


def _topo(seqs: list[list[str]], first: dict[str, int]) -> list[str] | None:
    """Keys in an order that keeps every sequence's order, ties by first[key]; None when the sequences contradict."""
    import heapq
    from itertools import pairwise

    after: dict[str, set] = {k: set() for k in first}
    indeg = dict.fromkeys(first, 0)
    for seq in seqs:
        for u, v in pairwise(seq):
            if u != v and v not in after[u]:
                after[u].add(v)
                indeg[v] += 1
    heap = [(first[k], k) for k, d in indeg.items() if d == 0]
    heapq.heapify(heap)
    out = []
    while heap:
        _, k = heapq.heappop(heap)
        out.append(k)
        for v in after[k]:
            indeg[v] -= 1
            if indeg[v] == 0:
                heapq.heappush(heap, (first[v], v))
    return out if len(out) == len(first) else None


def _merge_orders(calls: list[list]) -> list:
    """Assets of several discover() calls in one order that keeps every call's order (each call ranks its assets
    0..n-1 in the adapter's priority order): merge groups first (a project's priority shows through any of its
    tiles), then assets within a group. Keys that never share a call keep first-seen order: they only meet where no
    call's bounds reach, outside the scope. Contradicting calls fall back to first-seen order with a warning."""
    import logging

    ranked = [sorted(c, key=lambda x: x.rank) for c in calls]
    first: dict[str, int] = {}
    gfirst: dict[str, int] = {}
    by_id: dict = {}
    for call in ranked:
        for a in call:
            if a.id not in first:
                first[a.id] = len(first)
                by_id[a.id] = a
            gfirst.setdefault(a.group, len(gfirst))
    groups = _topo([[a.group for a in c] for c in ranked], gfirst)
    assets = _topo([[a.id for a in c] for c in ranked], first)
    if groups is None or assets is None:
        logging.getLogger(__name__).warning(
            "detail: the adapter's asset order differs between polygons; using first-seen order"
        )
        return sorted(by_id.values(), key=lambda a: first[a.id])
    gpos = {g: i for i, g in enumerate(groups)}
    apos = {k: i for i, k in enumerate(assets)}
    return sorted(by_id.values(), key=lambda a: (gpos[a.group], apos[a.id]))


def discover_in(src, scope) -> list:
    """src.discover() once per part of scope (on its bounds); the calls' assets merged into one order consistent with
    each call's ranks and renumbered 0..n-1, as one discover() over everything would rank them; assets whose metadata
    bbox misses the scope are dropped. src.version becomes the largest version any call reported."""
    from .sources.base import Area

    calls, versions = [], []
    for part in sorted(shapely.get_parts(scope).tolist(), key=lambda g: g.bounds):
        calls.append(src.discover(Area(tuple(float(v) for v in part.bounds))))
        versions.append(str(getattr(src, "version", "") or ""))
    if any(versions):
        src.version = max(versions)
    out = []
    for a in _merge_orders(calls):
        bb = a.metadata.get("bbox")
        if bb is not None and not shapely.intersects(shapely.box(*bb), scope):
            continue
        a.rank = len(out)
        out.append(a)
    return out


def inside(scope, lon, lat) -> np.ndarray:
    """True where (lon, lat) is in the scope or on its boundary."""
    return shapely.intersects_xy(scope, np.asarray(lon, np.float64), np.asarray(lat, np.float64))
