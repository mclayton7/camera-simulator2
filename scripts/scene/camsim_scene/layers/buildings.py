"""Buildings layer build (REALISM R3, R3b): snapshot -> plan_buildings (heights from the package's own bare-earth
terrain sources, the same height path verify --deep uses) -> one GLB per tile + tileset.json under features/buildings/,
plus the footprint mask (features/buildings/mask/) from the shared footprint raster. The plan and the raster are cached
under .state/ by their inputs, so the land-cover burn and this layer read the same buildings and plan them once.
Rebuilt as a whole (Pendleton ~82k buildings) and skipped when its inputs hash is unchanged."""

from __future__ import annotations

import json
import logging
import math
import pickle
import shutil
import time
from pathlib import Path

import numpy as np
import shapely

from .. import __version__
from .. import buildings as B
from .. import buildings_chunks as C
from .. import footprints as F
from ..context import BuildContext, WorkerState
from ..engine import LayerStats, inputs_hash, peak_rss_mb, run_pool
from ..fsutil import atomic_write, sha256_file
from ..gltf import write_glb
from ..licences import layer_credit
from ..manifest import STATE_DIR, Manifest, canonical_json
from ..roofs import RoofOptions
from ..sources.base import native_res_m
from . import imagery, terrain
from .landcover import tile_filename

log = logging.getLogger(__name__)

OUT = Path("features") / "buildings"
MASK = OUT / "mask"
MARKER = "buildings.json"
STATE_SUB = Path("buildings")  # under .state/: partition/, plan/<cx>_<cy>.{pkl,json}, plan.json
RASTER_DIR = "buildings-footprints"  # footprints.save_raster directory
# the pre-chunk whole-box plan and single-file raster
LEGACY_STATE = ("buildings-footprints.json", "buildings-footprints.npz", "buildings-plan.json", "buildings-plan.pkl")
_W: WorkerState | None = None  # the pool worker's state (init_worker)
TILESET_RULES_VERSION = "1"  # 1: z13 nodes outside the detail margins under ancestors (B.OUTSIDE_GEOMETRIC_ERROR)


def terrain_zoom(m: Manifest) -> int:
    return max(int(r["max_zoom"]["terrain"]) for r in m.regions)


def heights_fn(st: WorkerState, m: Manifest):
    """Bare-earth WGS-84 ellipsoid heights at points: `terrain.point_heights` is the exact-offset sampler verify --deep
    compares terrain tiles against; each source height gets its datum's vertical offset (geoid -> ellipsoid) added in
    `sample_group`, so the result is ellipsoid height like the terrain tiles themselves."""
    z = terrain_zoom(m)

    def fn(lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
        bounds = (float(lon.min()), float(lat.min()), float(lon.max()), float(lat.max()))
        entries = st.index["terrain"].query(bounds)
        if not entries:
            return np.full(lon.shape, np.nan)
        return terrain.point_heights(z, lon, lat, entries, st.tidal)

    return fn


class ImageryRoofSampler:
    """Roof pixels from the imagery layer's own sources (REALISM R3b): first valid in priority order, the colour match
    (imagery/balance.json) applied to its target source, as leaf tiles do. NAIP's edge feather and the water fade are
    leaf-tile blends and are not applied: a roof is textured only where a <= 2 m source (NAIP) supplies it."""

    def __init__(self, st: WorkerState):
        self.st = st
        self._res: dict[str, float] = {}

    def _native(self, entry) -> float:
        if entry.qid not in self._res:
            self._res[entry.qid] = native_res_m(entry.raster)
        return self._res[entry.qid]

    def sample(self, lon, lat, target_m):
        lon, lat = np.asarray(lon, np.float64), np.asarray(lat, np.float64)
        bounds = (float(lon.min()), float(lat.min()), float(lon.max()), float(lat.max()))
        entries = self.st.index["imagery"].query(bounds)
        if not entries:
            return np.zeros((3, *lon.shape), np.uint8), np.zeros(lon.shape, bool), np.full(lon.shape, np.inf)
        vals, ok, which = imagery.sample_entries(entries, lon, lat, target_m)
        rgb = imagery.decode_entries(entries, vals, which, lon, lat, self.st.balance)
        res = np.full(lon.shape, np.inf)
        for k, e in enumerate(entries):
            m = which == k
            if m.any():
                res[m] = self._native(e)
        return rgb, ok, res


def plan_from_manifest(m: Manifest, ctx: BuildContext, st: WorkerState | None = None):
    s = m.layers["buildings"]
    rec = m.source(s["priorities"][0])
    snapshot = Path(ctx.asset_paths[f"{rec.id}/{rec.assets[0].id}"])
    st = st or WorkerState(ctx)
    return B.plan_buildings(B.read_snapshot(snapshot), heights_fn(st, m), float(s["min_area_m2"]))


def footprint_inputs(m: Manifest, wst: WorkerState) -> str:
    """Everything the kept buildings depend on: the snapshot, min_area_m2, the rules, and the terrain (a building
    without a ground height is dropped)."""
    s = m.layers["buildings"]
    rec = m.source(s["priorities"][0])
    terrain_shas = [a.sha256 for sid in m.layers["terrain"]["priorities"] for a in m.source(sid).assets]  # in order
    return inputs_hash(
        __version__, B.BUILDINGS_RULES_VERSION, rec.assets[0].sha256, repr(float(s["min_area_m2"])),
        m.layer_settings_hash("terrain"), *wst.terrain_extra, *terrain_shas,
    )


def init_worker(ctx: BuildContext) -> None:
    global _W
    _W = WorkerState(ctx)


def _noop(*_):
    pass


def _state(pkg: Path) -> Path:
    return Path(pkg) / STATE_DIR / STATE_SUB


def _chunk_name(cx: int, cy: int) -> str:
    return f"{cx}_{cy}"


def _snapshot(m: Manifest, ctx: BuildContext) -> Path:
    rec = m.source(m.layers["buildings"]["priorities"][0])
    return Path(ctx.asset_paths[f"{rec.id}/{rec.assets[0].id}"])


def partition_inputs(m: Manifest) -> str:
    """What the partition depends on: the snapshot, min_area_m2 and the rules (not the terrain)."""
    s = m.layers["buildings"]
    rec = m.source(s["priorities"][0])
    return inputs_hash(__version__, B.BUILDINGS_RULES_VERSION, rec.assets[0].sha256, repr(float(s["min_area_m2"])))


def ensure_partition(pkg: Path, m: Manifest, ctx: BuildContext, jobs: int, out_dir: Path | None = None) -> dict:
    return C.partition(_snapshot(m, ctx), float(m.layers["buildings"]["min_area_m2"]),
                       Path(out_dir) if out_dir else _state(pkg) / "partition", partition_inputs(m), jobs)


def plan_chunk(st: WorkerState, m: Manifest, fps: list) -> tuple[list, dict]:
    return B.plan_buildings(fps, heights_fn(st, m), float(m.layers["buildings"]["min_area_m2"]))


def _plan_one(item) -> list[dict]:
    pkg, cx, cy, key = item
    blds, stats = plan_chunk(_W, _W.manifest, C.read_chunk(_state(pkg) / "partition", cx, cy))
    d = _state(pkg) / "plan"
    atomic_write(d / f"{_chunk_name(cx, cy)}.pkl", pickle.dumps(blds, protocol=pickle.HIGHEST_PROTOCOL))
    rec = {"inputs": key, "kept": stats["kept"], "dropped_no_height": stats["dropped_no_height"]}
    atomic_write(d / f"{_chunk_name(cx, cy)}.json", json.dumps(rec).encode())  # after the pickle: the chunk's marker
    return [{"chunk": [cx, cy], **rec, "rss_mb": peak_rss_mb()}]


def plan_batch(batch) -> list[dict]:
    return [r for it in batch for r in _plan_one(it)]


def load_chunk_plan(pkg: Path, cx: int, cy: int) -> list:
    return pickle.loads((_state(pkg) / "plan" / f"{_chunk_name(cx, cy)}.pkl").read_bytes())


def plan_chunks(pkg: Path, m: Manifest, ctx: BuildContext, jobs: int, wst: WorkerState | None = None):
    """Partition (cached), then plan every chunk whose cached plan is missing or stale, in the pool. Returns
    (chunks, stats, peak RSS MB); stats has plan_buildings' keys in its order."""
    wst = wst or WorkerState(ctx)
    for name in LEGACY_STATE:
        (Path(pkg) / STATE_DIR / name).unlink(missing_ok=True)
    part = ensure_partition(pkg, m, ctx, jobs)
    chunks = [tuple(c) for c in part["chunks"]]
    base = footprint_inputs(m, wst)
    d = _state(pkg) / "plan"
    d.mkdir(parents=True, exist_ok=True)
    keep = {f"{_chunk_name(*c)}{ext}" for c in chunks for ext in (".pkl", ".json")}
    for p in d.iterdir():
        if p.name not in keep:
            p.unlink()
    todo, recs, peak = [], {}, part["peak_rss_mb"]
    for c in chunks:
        key = inputs_hash(base, _chunk_name(*c))
        mk, pk = d / f"{_chunk_name(*c)}.json", d / f"{_chunk_name(*c)}.pkl"
        rec = json.loads(mk.read_text()) if mk.exists() else None
        if rec and rec.get("inputs") == key and pk.exists():
            recs[c] = rec
        else:
            todo.append((str(pkg), *c, key))

    def on_result(r):  # results arrive in completion order with jobs > 1: key them by chunk
        nonlocal peak
        recs[tuple(r["chunk"])] = r
        peak = max(peak, r["rss_mb"])

    run_pool(plan_batch, [[[t] for t in todo]], jobs, init_worker, (ctx,), on_result)
    counts = part["counts"]
    stats = {
        "input": counts["input"], "kept": sum(r["kept"] for r in recs.values()),
        "dropped_small": counts["dropped_small"], "dropped_invalid": counts["dropped_invalid"],
        "dropped_no_height": sum(r["dropped_no_height"] for r in recs.values()),
    }
    atomic_write(_state(pkg) / "plan.json", json.dumps({"inputs": base, "stats": stats}).encode())
    return chunks, stats, peak


def _raster_one(item) -> list[dict]:
    pkg, cx, cy = item
    return [{"tiles": F.raster_tiles(load_chunk_plan(pkg, cx, cy)), "rss_mb": peak_rss_mb()}]


def raster_batch(batch) -> list[dict]:
    return [r for it in batch for r in _raster_one(it)]


def footprint_raster(pkg: Path, m: Manifest, ctx: BuildContext, wst: WorkerState | None = None,
                     jobs: int = 1) -> tuple[Path, str, float]:
    """The cached footprint raster directory of the kept buildings, its inputs key and the peak RSS (MB) spent on it:
    each chunk rasterised in the pool, OR-merged per land-cover tile in this process (spec section 4.3)."""
    wst = wst or WorkerState(ctx)
    key = inputs_hash(footprint_inputs(m, wst), F.MASK_RULES_VERSION)
    out = Path(pkg) / STATE_DIR / RASTER_DIR
    if F.raster_key(out) == key:
        return out, key, 0.0
    chunks, _, peak = plan_chunks(pkg, m, ctx, jobs, wst)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)

    def on_result(r):
        nonlocal peak
        for ij, packed in r["tiles"].items():
            F.or_tile(out, ij, packed)
        peak = max(peak, r["rss_mb"])

    run_pool(raster_batch, [[[(str(pkg), *c)] for c in chunks]], jobs, _noop, (), on_result)
    F.write_raster_index(out, key)
    return out, key, peak


def write_mask(pkg: Path, raster: F.FootprintRaster) -> str:
    """features/buildings/mask/: one 1-bit PNG per raster tile + index.json; returns the index's sha256 ("" = none)."""
    written = raster.keys()
    if not written:
        return ""
    d = Path(pkg) / MASK
    for i, j in written:
        F.write_mask_tile(raster.get(i, j), d / tile_filename(i, j))
    atomic_write(d / "index.json", F.mask_index_bytes(written))
    return sha256_file(d / "index.json")


def _mask_intact(pkg: Path, sha: str) -> bool:
    index = Path(pkg) / MASK / "index.json"
    if not sha:
        return not index.exists()
    if not index.exists() or sha256_file(index) != sha:
        return False
    return all((index.parent / t["file"]).is_file() for t in json.loads(index.read_text())["tiles"])


def near_only_fn(m: Manifest):
    """near_only for B.tileset_json: a z13 node whose region misses every detail_margin:* region (REALISM R5) is drawn
    only near the camera. None without detail polygons: the tileset is the plain R3 tree."""
    margins = [shapely.Polygon(r["polygon"], r.get("holes") or None) for r in m.regions
               if r["name"].startswith("detail_margin:")]
    if not margins:
        return None
    union = shapely.union_all(margins)

    def near_only(region) -> bool:
        w, s, e, n = (math.degrees(v) for v in region[:4])
        return not union.intersects(shapely.box(w, s, e, n))

    return near_only


def _glb_one(item) -> list[dict]:
    """One chunk's GLBs (every tile of a chunk's buildings lies in that chunk) and their tileset summaries."""
    pkg, cx, cy, credit = item
    st = _W
    s = st.manifest.layers["buildings"]
    roof = RoofOptions.from_settings(s) if s.get("roof_textures") else None
    sampler = ImageryRoofSampler(st) if roof is not None else None
    by: dict = {}
    for b in load_chunk_plan(pkg, cx, cy):
        by.setdefault(b.tile, []).append(b)
    out, tiles, roofs = Path(pkg) / OUT, {}, {}
    for key, bl in sorted(by.items()):  # tiles only exist for keys with a building: write_glb rejects empty meshes
        tiles[key] = B.tile_summary(key, bl)
        mesh, table = B.tile_mesh(bl, tiles[key][1], roof, sampler, roofs)
        z, x, y = key
        p = out / str(z) / str(x) / f"{y}.glb"
        p.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(p, write_glb(mesh, table, credit))
    return [{"tiles": tiles, "roofs": roofs, "rss_mb": peak_rss_mb()}]


def glb_batch(batch) -> list[dict]:
    return [r for it in batch for r in _glb_one(it)]


def plan_stats(pkg: Path) -> dict | None:
    marker = Path(pkg) / STATE_DIR / MARKER
    return json.loads(marker.read_text())["stats"] if marker.exists() else None


def build(pkg: Path, m: Manifest, ctx: BuildContext, jobs: int = 1) -> LayerStats:
    st, out = LayerStats(), Path(pkg) / OUT
    s = m.layers.get("buildings")
    if not s:
        shutil.rmtree(Path(pkg) / "features", ignore_errors=True)
        for name in (MARKER, *LEGACY_STATE):
            (Path(pkg) / STATE_DIR / name).unlink(missing_ok=True)
        shutil.rmtree(Path(pkg) / STATE_DIR / RASTER_DIR, ignore_errors=True)
        shutil.rmtree(_state(pkg), ignore_errors=True)
        return st
    t0 = time.monotonic()
    rec = m.source(s["priorities"][0])
    wst = WorkerState(ctx)
    credit = layer_credit(rec.attribution, rec.licence, layer="buildings")
    inputs = inputs_hash(m.layer_settings_hash("buildings"), __version__, rec.assets[0].sha256, credit,
                         footprint_inputs(m, wst), TILESET_RULES_VERSION)
    roof = RoofOptions.from_settings(s) if s.get("roof_textures") else None
    if roof is not None:  # roof pixels come from the imagery sources and the colour match: both are inputs
        img_shas = [a.sha256 for sid in m.layers["imagery"]["priorities"] for a in m.source(sid).assets]
        inputs = inputs_hash(inputs, m.layer_settings_hash("imagery"), wst.balance_sha, *img_shas)
    marker, ts_path = Path(pkg) / STATE_DIR / MARKER, out / "tileset.json"
    prev = json.loads(marker.read_text()) if marker.exists() else None
    if prev and prev["inputs"] == inputs and not prev["output"] and not out.exists():
        st.seconds = round(time.monotonic() - t0, 3)  # no building survived planning: nothing on disk
        return st
    if (
        prev and prev["inputs"] == inputs and prev["output"] and ts_path.exists()
        and sha256_file(ts_path) == prev["output"] and _mask_intact(pkg, prev.get("mask", ""))
    ):
        uris = [u for u, _ in B.walk_tileset(json.loads(ts_path.read_text()))]
        if all((out / u).is_file() for u in uris):
            files = [p for p in out.rglob("*") if p.is_file()]
            st.tiles = st.skipped = len(uris)
            st.files, st.bytes = len(files), sum(p.stat().st_size for p in files)
            st.seconds = round(time.monotonic() - t0, 3)
            return st
    shutil.rmtree(out, ignore_errors=True)
    chunks, stats, peak = plan_chunks(pkg, m, ctx, jobs, wst)
    tiles: dict = {}
    roof_stats: dict = {}

    def on_result(r):
        nonlocal peak
        tiles.update(r["tiles"])
        for k, v in r["roofs"].items():
            if k == "tiles_by_texel_m":
                by = roof_stats.setdefault(k, {})
                for t, n in v.items():
                    by[t] = by.get(t, 0) + n
            else:
                roof_stats[k] = roof_stats.get(k, 0) + v
        peak = max(peak, r["rss_mb"])

    flat = credit.replace("\n", " ")
    run_pool(glb_batch, [[[(str(pkg), *c, flat)] for c in chunks]], jobs, init_worker, (ctx,), on_result)
    tiles = dict(sorted(tiles.items()))
    if "tiles_by_texel_m" in roof_stats:
        roof_stats["tiles_by_texel_m"] = dict(sorted(roof_stats["tiles_by_texel_m"].items()))
    if roof is not None:  # stats is the cached plan's dict: copy it, never mutate it
        stats = {**stats, "roofs": {"textured": 0, "flat_coarse": 0, "flat_overflow": 0, "tiles_by_texel_m": {},
                                    **roof_stats}}
    output = mask_sha = ""  # no tileset and no mask when nothing survived planning
    if tiles:
        out.mkdir(parents=True, exist_ok=True)
        atomic_write(ts_path, canonical_json(B.tileset_json(tiles, near_only_fn(m))).encode())
        output = sha256_file(ts_path)
        if s.get("mask"):  # R3b; an R3 manifest (no "mask" settings) builds exactly as before
            raster_path, _, raster_peak = footprint_raster(pkg, m, ctx, wst, jobs)
            peak = max(peak, raster_peak)
            mask_sha = write_mask(pkg, F.FootprintRaster(raster_path))
    atomic_write(marker, json.dumps({"inputs": inputs, "output": output, "mask": mask_sha, "stats": stats}).encode())
    files = [p for p in out.rglob("*") if p.is_file()]
    st.tiles = st.built = len(tiles)
    st.files, st.bytes = len(files), sum(p.stat().st_size for p in files)
    st.peak_rss_mb = max(peak_rss_mb(), peak)
    st.seconds = round(time.monotonic() - t0, 3)
    return st
