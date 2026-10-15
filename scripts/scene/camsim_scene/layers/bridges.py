"""Bridges layer (REALISM R3 bridges, spec docs/superpowers/specs/2026-10-15-bridges-ripples-design.md section 1.4):
R2's OSM bridge ways -> decks (heights from the package's bare-earth terrain at grounded ends) -> features/bridges/
(3D Tiles 1.1, one GLB per z13 tile, content-less z10 groups). Hundreds of ways, so it runs in the main process; it is
skipped when its inputs hash is unchanged."""

from __future__ import annotations

import json
import math
import shutil
import time
from pathlib import Path

import numpy as np

from .. import __version__
from .. import bridges as BR
from .. import geodesy
from ..buildings import tile_of
from ..context import BuildContext, WorkerState
from ..engine import LayerStats, inputs_hash, peak_rss_mb
from ..fsutil import atomic_write, sha256_file
from ..licences import layer_credit
from ..manifest import STATE_DIR, Manifest, canonical_json
from ..tiling import tile_bounds
from . import buildings as buildings_layer

OUT = Path("features") / "bridges"
MARKER = "bridges.json"


def terrain_inputs(m: Manifest, wst: WorkerState) -> str:
    """The deck heights' terrain: the terrain layer settings, its extra inputs and every terrain asset, in order."""
    shas = [a.sha256 for sid in m.layers["terrain"]["priorities"] for a in m.source(sid).assets]
    return inputs_hash(m.layer_settings_hash("terrain"), *wst.terrain_extra, *shas)


def _credit(m: Manifest) -> str:
    rec = m.source(m.layers["bridges"]["source"])
    return layer_credit(rec.attribution, rec.licence, layer="bridges")


def remove(pkg: Path, out: Path, marker: Path) -> None:
    """Drop a feature layer's output and marker; features/ goes too once empty (buildings and bridges share it)."""
    shutil.rmtree(Path(pkg) / out, ignore_errors=True)
    marker.unlink(missing_ok=True)
    feats = Path(pkg) / "features"
    if feats.is_dir() and not any(feats.iterdir()):
        feats.rmdir()


def _region(decks: list[BR.Deck], heights_fn) -> list[float]:
    """Bounding region (radians, m) of the decks with parapets and piers: footprint padded by half width + parapet, low
    = the terrain along the decks - pier sink (or the deck bottom), high = deck top + parapet."""
    w = s = math.inf
    e = n = -math.inf
    lo, hi = math.inf, -math.inf
    for d in decks:
        lon, lat, _ = BR._samples(d.line, BR.PROFILE_STEP_M)
        sx, sy = BR._scale(float(lat.mean()))
        pad = (d.width_m / 2.0 + BR.PARAPET_W_M) * BR.MAX_MITRE
        w, e = min(w, lon.min() - pad / sx), max(e, lon.max() + pad / sx)
        s, n = min(s, lat.min() - pad / sy), max(n, lat.max() + pad / sy)
        terr = np.asarray(heights_fn(lon, lat), float)
        low_deck = min(d.h0, d.h1) - BR.DECK_THICKNESS_M
        lo = min(lo, low_deck, float(np.nanmin(terr)) - BR.PIER_SINK_M if np.isfinite(terr).any() else low_deck)
        hi = max(hi, max(d.h0, d.h1) + BR.PARAPET_H_M)
    return [math.radians(w), math.radians(s), math.radians(e), math.radians(n), round(lo, 3), round(hi, 3)]


def _union(regions: list[list[float]]) -> list[float]:
    r = np.asarray(regions)
    return [float(r[:, 0].min()), float(r[:, 1].min()), float(r[:, 2].max()), float(r[:, 3].max()),
            float(r[:, 4].min()), float(r[:, 5].max())]


def tile_key(d: BR.Deck) -> tuple[int, int, int]:
    mid = d.line.interpolate(0.5, normalized=True)
    return (BR.TILE_ZOOM, *tile_of(mid.x, mid.y, BR.TILE_ZOOM))


def tile_origin(key: tuple[int, int, int], decks: list[BR.Deck]) -> tuple[float, float, float]:
    w, s, e, n = tile_bounds(*key)
    hs = [h for d in decks for h in (d.h0, d.h1)]
    return (w + e) / 2.0, (s + n) / 2.0, round((min(hs) + max(hs)) / 2.0, 3)


def tileset_json(tiles: dict) -> dict:
    """tiles: (z, x, y) -> (region, ENU origin). Root (always refined) -> content-less z10 groups (GROUP_GEOMETRIC_ERROR)
    -> z13 content tiles (geometric error 0, ADD), each with its absolute ENU transform."""
    groups: dict = {}
    for key in sorted(tiles):
        z, x, y = key
        d = z - BR.GROUP_ZOOM
        groups.setdefault((x >> d, y >> d), []).append(key)
    kids = []
    for g in sorted(groups):
        leaves = []
        for key in groups[g]:
            region, origin = tiles[key]
            z, x, y = key
            leaves.append({
                "boundingVolume": {"region": region}, "geometricError": 0.0,
                "transform": geodesy.matrix_to_column_major(geodesy.enu_to_ecef_matrix(*origin)),
                "content": {"uri": f"{z}/{x}/{y}.glb"},
            })
        kids.append({"boundingVolume": {"region": _union([t["boundingVolume"]["region"] for t in leaves])},
                     "geometricError": BR.GROUP_GEOMETRIC_ERROR, "children": leaves})
    root = {"boundingVolume": {"region": _union([k["boundingVolume"]["region"] for k in kids])},
            "geometricError": BR.ROOT_GEOMETRIC_ERROR, "refine": "ADD", "children": kids}
    return {"asset": {"version": "1.1", "generator": f"camsim-scene {__version__}"},
            "geometricError": BR.ROOT_GEOMETRIC_ERROR, "root": root}


def plan_stats(pkg: Path) -> dict | None:
    marker = Path(pkg) / STATE_DIR / MARKER
    return json.loads(marker.read_text())["stats"] if marker.exists() else None


def plan(m: Manifest, ctx: BuildContext, features: list, wst: WorkerState | None = None):
    """(decks, stats, heights_fn) from R2's features and the package's terrain."""
    wst = wst or WorkerState(ctx)
    hf = buildings_layer.heights_fn(wst, m)
    decks, stats = BR.plan_decks(features, hf, float(m.layers["bridges"]["min_clearance_m"]))
    return decks, stats, hf


def build(pkg: Path, m: Manifest, ctx: BuildContext, features_fn, features_key: str) -> LayerStats:
    """features_fn() -> R2's feature list (called only when the layer rebuilds); features_key: its cache key."""
    st, out = LayerStats(), Path(pkg) / OUT
    marker = Path(pkg) / STATE_DIR / MARKER
    s = m.layers.get("bridges")
    if not s:
        remove(pkg, OUT, marker)
        return st
    t0 = time.monotonic()
    wst = WorkerState(ctx)
    credit = _credit(m)
    inputs = inputs_hash(m.layer_settings_hash("bridges"), __version__, BR.BRIDGES_RULES_VERSION, features_key,
                         terrain_inputs(m, wst), credit)
    ts_path = out / "tileset.json"
    prev = json.loads(marker.read_text()) if marker.exists() else None
    if prev and prev["inputs"] == inputs:
        if not prev["output"] and not out.exists():
            st.seconds = round(time.monotonic() - t0, 3)
            return st
        if prev["output"] and ts_path.exists() and sha256_file(ts_path) == prev["output"]:
            uris = [u for u, _ in walk_tileset(json.loads(ts_path.read_text()))]
            if all((out / u).is_file() for u in uris):
                files = [p for p in out.rglob("*") if p.is_file()]
                st.tiles = st.skipped = len(uris)
                st.files, st.bytes = len(files), sum(p.stat().st_size for p in files)
                st.seconds = round(time.monotonic() - t0, 3)
                return st
    shutil.rmtree(out, ignore_errors=True)
    decks, stats, hf = plan(m, ctx, features_fn(), wst)
    by: dict = {}
    for d in decks:
        by.setdefault(tile_key(d), []).append(d)
    flat = credit.replace("\n", " ")
    tiles = {}
    for key in sorted(by):
        ds = by[key]
        origin = tile_origin(key, ds)
        z, x, y = key
        p = out / str(z) / str(x) / f"{y}.glb"
        p.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(p, BR.bridges_glb(ds, origin, hf, flat))
        tiles[key] = (_region(ds, hf), origin)
    output = ""
    if tiles:
        atomic_write(ts_path, canonical_json(tileset_json(tiles)).encode())
        output = sha256_file(ts_path)
    atomic_write(marker, json.dumps({"inputs": inputs, "output": output, "stats": stats}).encode())
    files = [p for p in out.rglob("*") if p.is_file()] if out.exists() else []
    st.tiles = st.built = len(tiles)
    st.files, st.bytes = len(files), sum(p.stat().st_size for p in files)
    st.peak_rss_mb = peak_rss_mb()
    st.seconds = round(time.monotonic() - t0, 3)
    return st


def walk_tileset(doc: dict) -> list[tuple[str, np.ndarray]]:
    from ..buildings import walk_tileset as walk

    return walk(doc)
