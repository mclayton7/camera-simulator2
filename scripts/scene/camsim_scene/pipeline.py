"""plan -> fetch -> build.

plan:  discover each source's assets for its area (globe / ring / bbox) and write manifest.json (unhashed).
fetch: download whole assets into the cache, hash them, prepare derived files, compute footprints, hash grids.
build: plan the pyramids, run the tile jobs (resumable), then write layer.json, tilemapresource.xml (imagery, NDVI),
       land cover, ATTRIBUTION.txt, hashes.txt, the final manifest.json and build.json."""

from __future__ import annotations

import contextlib
import datetime as dt
import json
import logging
import multiprocessing
import os
import platform
import shutil
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from functools import partial
from pathlib import Path

import numpy as np
import shapely

from . import __version__, datum, ndvi_fit, sea_level as sea_level_mod
from . import balance as colour_balance
from .cache import Cache
from .config import LAYERS, TILING, ScenePlan, layer_settings
from .context import BuildContext, WorkerState, class_rasters, coverage, reference_footprints, to_asset
from .engine import (
    BuildLock,
    LayerStats,
    Markers,
    Progress,
    inputs_hash,
    peak_rss_mb,
    remove_stale,
    run_pool,
    run_tile,
    tile_path,
)
from .fsutil import atomic_write, normalise_modes, sha256_file
from .layers import imagery, landcover, ndvi, terrain
from .licences import DEFAULT_ALLOW, attribution_text, check_allowed
from .manifest import STATE_DIR, AssetRecord, Manifest, SourceRecord, canonical_json, write_hashes
from .net import Http
from .qmesh import layer_json
from .sources import make_source
from .sources.base import Area, Layer, footprint_lonlat
from .tiling import LayerPlan, available_ranges, ndvi_plan, plan_tiles, split_keys
from .tms import plan_bounds, tilemapresource_xml
from .verify import present_tiles

log = logging.getLogger("camsim_scene")
TOOL = {"name": "camsim-scene", "version": __version__}
CHUNK = 64
EXT = {"terrain": "terrain", "imagery": "jpg", "ndvi": "png"}
EST_KB = {
    "terrain": 7.5,
    "imagery": 16.0,
    "landcover": 15.0,  # per tile, from the R0 spike
    "ndvi": 20.0,  # 256 px 8-bit grayscale PNG: conservative (smooth NDVI compresses well, noisy tiles reach ~40 KB)
}


class PlanError(Exception):
    pass


class BuildError(Exception):
    pass


def _utc_now() -> str:
    return dt.datetime.now(dt.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")


def _lock(pkg: Path, locked: bool):
    """The package's build lock, unless the caller already holds it (`locked`): flock is per open file, so taking
    it twice in one process would refuse the second time."""
    if locked:
        return contextlib.nullcontext()
    Path(pkg).mkdir(parents=True, exist_ok=True)
    return BuildLock(pkg)


# ---------------------------------------------------------------- plan


def plan_scene(plan: ScenePlan, pkg: Path, http=None, out=sys.stderr, locked: bool = False) -> Manifest:
    """Discover every source's assets and write manifest.json (under the build lock: a running build refuses)."""
    http = http or Http()
    built: dict[str, tuple[str, object]] = {}
    for layer in LAYERS:
        for sid in plan.priorities[layer]:
            if sid in built:
                raise PlanError(f"source {sid} is listed under more than one layer")
            opts = dict(plan.source_options.get(sid, {}))
            adapter = opts.pop("adapter", sid)
            src = make_source(sid, adapter, opts, http)
            if src.layer != layer:
                raise PlanError(f"source {sid} is a {src.layer} source but is listed under priorities.{layer}")
            built[sid] = (adapter, src)
    allow = sorted(set(DEFAULT_ALLOW) | set(plan.allow))
    try:
        sea = sea_level_mod.station_section(http, plan.sea_level_station) if plan.sea_level_station else None
    except sea_level_mod.SeaLevelError as e:
        raise PlanError(str(e)) from e
    check_allowed({sid: s.licence for sid, (_, s) in built.items()}, allow)
    records, datums = [], set()
    if sea:
        datums.add(sea["geoid"])  # the station's NAVD88 geoid grid is pinned and hashed with the rest
    for sid, (adapter, src) in built.items():
        assets = src.discover(Area(plan.area(src.area_kind)))
        if not assets:
            log.warning("%s: no assets in %s", sid, plan.area(src.area_kind))
        if assets and getattr(src, "vertical_from_msl", False):
            if sea is None:
                raise PlanError(
                    f"{sid} heights are MSL: the scene needs a [sea_level] station (a NOAA CO-OPS id with a NAVD88 "
                    'tie, e.g. station = "9410230") to convert them'
                )
            for a in assets:
                a.metadata["msl_above_navd88_m"] = sea["msl_above_navd88_m"]
        for a in assets:
            if a.role == "data" and src.layer != Layer.LANDCOVER and (a.metadata.get("datum") or src.datum):
                datums.add(a.metadata.get("datum") or src.datum)
        records.append(
            SourceRecord(
                sid,
                adapter,
                src.dataset,
                src.version,
                src.licence,
                src.attribution,
                src.options(),
                [AssetRecord(a.id, a.url, None, a.size, a.group, a.rank, a.role, dict(a.metadata)) for a in assets],
            )
        )
    m = Manifest(
        name=plan.name,
        bbox=list(plan.bbox),
        seed=plan.seed,
        regions=[r.to_dict() for r in plan.regions()],
        tiling=TILING,
        layers=layer_settings(plan),
        datum=datum.manifest_section(datums),
        sources=records,
        licence_allow=allow,
        tool=TOOL,
        sea_level=sea,
    )
    with _lock(pkg, locked):
        m.write(Path(pkg) / "manifest.json")
    est = estimate(m)
    unknown = f" (+{est['unknown_sizes']} assets of unknown size)" if est["unknown_sizes"] else ""
    print(
        f"plan {m.name}: {sum(len(s.assets) for s in m.sources)} assets, cache {est['cache_bytes'] / 1e9:.1f} GB{unknown}; "
        f"tiles terrain {est['tiles']['terrain']}, imagery {est['tiles']['imagery']}, land cover {est['tiles']['landcover']}, ndvi {est['tiles']['ndvi']}; "
        f"package ~{est['package_bytes'] / 1e9:.1f} GB",
        file=out,
    )
    return m


LAYERS_EST = ("terrain", "imagery")


def estimate(m: Manifest) -> dict:
    sizes = [a.size for s in m.sources for a in s.assets]
    use_bbox = not m.is_fetched()
    plans = {layer: plan_tiles(m.region_objs(), layer, coverage(m, layer, use_bbox=use_bbox)) for layer in LAYERS_EST}
    tiles = {layer: p.count() for layer, p in plans.items()}
    lc = m.layers["landcover"]
    tiles["landcover"] = len(landcover.tiles_for_bbox(*lc["bounds"])) if lc["priorities"] else 0
    nd = m.layers.get("ndvi")
    tiles["ndvi"] = ndvi_plan(plans["imagery"], nd["max_zoom"], _ndvi_area(m, use_bbox)).count() if nd else 0
    package = sum(tiles[k] * EST_KB[k] * 1024 for k in tiles)
    return {
        "cache_bytes": sum(v for v in sizes if v),
        "unknown_sizes": sum(1 for v in sizes if v is None),
        "tiles": tiles,
        "package_bytes": int(package),
    }


# ---------------------------------------------------------------- fetch


def _cached_assets(rec: SourceRecord, cache: Cache):
    """Yield (source, asset record, local path) for each of rec's assets: the prepared file when the adapter derives
    one (COG with overviews), else the cached blob. Downloads misses (signing through the adapter, so an expired
    token is re-signed), records sha256 and size, and fails on a pinned-hash mismatch."""
    src = make_source(rec.id, rec.adapter, rec.options, cache.http)
    for a in rec.assets:
        blob, a.sha256, a.size = cache.get(a.url, a.sha256, sign=src.sign)
        yield src, a, src.prepare(blob, to_asset(a), cache) or blob


def plan_differences(plan: ScenePlan, m: Manifest) -> list[str]:
    """Manifest fields that re-planning `plan` would change (inputs only: discovery isn't re-run)."""

    def norm(v):
        return json.loads(canonical_json(v))

    def source_options(sid: str):
        opts = dict(plan.source_options.get(sid, {}))
        return {"adapter": opts.pop("adapter", sid), "options": opts}

    def manifest_options(sid: str):
        try:
            rec = m.source(sid)
        except KeyError:
            return None
        return {"adapter": rec.adapter, "options": rec.options}

    want = {
        "name": plan.name,
        "seed": plan.seed,
        "regions": [r.to_dict() for r in plan.regions()],
        "layers": layer_settings(plan),
        "licence_allow": sorted(set(DEFAULT_ALLOW) | set(plan.allow)),
        "sources": {sid: source_options(sid) for sid in plan.source_ids()},
        "sea_level_station": plan.sea_level_station,
    }
    have = {
        "name": m.name,
        "seed": m.seed,
        "regions": m.regions,
        "layers": m.layers,
        "licence_allow": m.licence_allow,
        "sources": {sid: manifest_options(sid) for sid in plan.source_ids()},
        "sea_level_station": (m.sea_level or {}).get("station"),
    }
    return [k for k in want if norm(want[k]) != norm(have[k])]


def _needs_footprints(m: Manifest) -> bool:
    """A non-global data asset of a terrain/imagery source without a footprint (fetch computes them)."""
    for rec in m.sources:
        src = make_source(rec.id, rec.adapter, rec.options)
        if src.layer == Layer.LANDCOVER or src.global_coverage:
            continue
        if any(a.role == "data" and "footprint" not in a.metadata for a in rec.assets):
            return True
    return False


def needs_fetch(m: Manifest) -> bool:
    return not m.is_fetched() or _needs_footprints(m)


def fetch_scene(pkg: Path, cache: Cache, locked: bool = False) -> Manifest:
    """Download, hash and footprint every asset, rewriting manifest.json as it goes (under the build lock)."""
    with _lock(pkg, locked):
        return _fetch(Path(pkg), cache)


def _fetch(pkg: Path, cache: Cache) -> Manifest:
    m = Manifest.load(pkg / "manifest.json")
    for rec in m.sources:
        for src, a, path in _cached_assets(rec, cache):
            if (
                a.role == "data"
                and src.layer != Layer.LANDCOVER
                and not src.global_coverage
                and "footprint" not in a.metadata
            ):
                fp = footprint_lonlat(src.open(path, to_asset(a)))
                a.metadata["footprint"] = None if fp is None else shapely.to_wkt(fp, rounding_precision=7)
                if fp is None:
                    log.warning("%s/%s holds no data; it is ignored", rec.id, a.id)
            log.info("fetched %s/%s (%d bytes)", rec.id, a.id, a.size)
        m.write(pkg / "manifest.json")  # an interrupted fetch keeps what it already hashed
    for g in m.datum["grids"].values():
        _, g["sha256"], _ = cache.get(g["url"], g.get("sha256"))
    if m.sea_level:
        g = m.sea_level["egm96_grid"]
        _, g["sha256"], _ = cache.get(g["url"], g.get("sha256"))
    m.write(pkg / "manifest.json")
    return m


def make_context(pkg: Path, m: Manifest, cache: Cache) -> BuildContext:
    paths = {f"{rec.id}/{a.id}": str(path) for rec in m.sources for _, a, path in _cached_assets(rec, cache)}
    grids = {name: str(cache.get(g["url"], g["sha256"])[0]) for name, g in m.datum["grids"].items()}
    if m.sea_level:
        g = m.sea_level["egm96_grid"]
        grids[g["name"]] = str(cache.get(g["url"], g["sha256"])[0])
    return BuildContext(str(pkg), m.to_dict(), paths, grids)


# ---------------------------------------------------------------- workers

_STATE: WorkerState | None = None
_MARKERS: Markers | None = None


def init_worker(ctx: BuildContext) -> None:
    global _STATE, _MARKERS
    os.environ.setdefault("GDAL_CACHEMAX", "256")
    _STATE = WorkerState(ctx)
    _MARKERS = Markers(_STATE.pkg)


def _terrain_tile(z: int, x: int, y: int):
    st = _STATE
    entries = st.index["terrain"].query(terrain.query_bounds(z, x, y))
    inputs = inputs_hash(st.settings["terrain"], __version__, *st.terrain_extra, *sorted(e.sha256 for e in entries))
    return run_tile(
        st.pkg, _MARKERS, "terrain", "terrain", z, x, y, inputs, partial(terrain.build_tile, z, x, y, entries, st.tidal)
    )


def terrain_batch(keys: list[tuple[int, int, int]]) -> list:
    return [_terrain_tile(*k) for k in keys]


def _parent_bytes(pkg: Path, z: int, x: int, y: int, quality: int) -> bytes:
    kids = {
        (dx, dy): tile_path(pkg, "imagery", z + 1, 2 * x + dx, 2 * y + dy, "jpg").read_bytes()
        for dx in (0, 1)
        for dy in (0, 1)
    }
    return imagery.parent_tile(kids, quality)


def _imagery_tile(z: int, x: int, y: int, leaf: bool):
    st = _STATE
    quality = st.manifest.layers["imagery"]["jpeg_quality"]
    if leaf:
        bal = st.balance
        entries = st.index["imagery"].query(imagery.query_bounds(z, x, y, bal.feather_m if bal else 0.0))
        # balance_sha only when there is a balance: leaves of packages without one keep their pre-balance hashes;
        # the WorldCover hashes only with a water mask, so a WorldCover change rebuilds the leaves
        extra = [st.balance_sha, *st.water_shas] if st.balance_sha else []
        inputs = inputs_hash(st.settings["imagery"], __version__, "leaf", *extra, *sorted(e.sha256 for e in entries))
        produce = partial(imagery.leaf_tile, z, x, y, entries, quality, bal, st.ref_interior, st.water)
    else:
        outs = []
        for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
            rec = _MARKERS.read("imagery", z + 1, 2 * x + dx, 2 * y + dy)
            if rec is None:
                raise BuildError(f"imagery {z}/{x}/{y}: child {z + 1}/{2 * x + dx}/{2 * y + dy} was not built")
            outs.append(rec["output"])
        inputs = inputs_hash(st.settings["imagery"], __version__, "parent", *outs)
        produce = partial(_parent_bytes, st.pkg, z, x, y, quality)
    return run_tile(st.pkg, _MARKERS, "imagery", "jpg", z, x, y, inputs, produce)


def imagery_batch(items: list[tuple[int, int, int, bool]]) -> list:
    return [_imagery_tile(*it) for it in items]


def _ndvi_parent_bytes(pkg: Path, z: int, x: int, y: int) -> bytes | None:
    kids = {}
    for dx in (0, 1):
        for dy in (0, 1):
            p = tile_path(pkg, "ndvi", z + 1, 2 * x + dx, 2 * y + dy, "png")
            kids[(dx, dy)] = p.read_bytes() if p.exists() else None
    return ndvi.parent_tile(kids)


NDVI_KIDS = ((0, 0), (1, 0), (0, 1), (1, 1))


def _ndvi_child_outputs(read, z: int, x: int, y: int, planned: tuple[bool, ...]) -> list[str]:
    """The four children's output hashes for a parent's inputs. A child outside the NDVI plan (beyond the NIR area)
    hashes as "absent"; a planned child with no marker was never built, which is an error."""
    outs = []
    for (dx, dy), is_planned in zip(NDVI_KIDS, planned, strict=True):
        rec = read("ndvi", z + 1, 2 * x + dx, 2 * y + dy)
        if rec is None and is_planned:
            raise BuildError(f"ndvi {z}/{x}/{y}: child {z + 1}/{2 * x + dx}/{2 * y + dy} was not built")
        outs.append(rec["output"] if rec else "absent")
    return outs


def _ndvi_tile(z: int, x: int, y: int, leaf: bool, planned: tuple[bool, ...] = ()):
    st = _STATE
    s = st.manifest.layers["ndvi"]
    if leaf:
        entries = st.index["ndvi"].query(imagery.query_bounds(z, x, y, s["feather_m"]))
        extra = ([st.ndvi_fit_sha] if st.ndvi_fit_sha else []) + st.ndvi_water_shas
        inputs = inputs_hash(st.settings["ndvi"], __version__, "leaf", *extra, *sorted(e.sha256 for e in entries))
        produce = partial(
            ndvi.leaf_tile,
            z,
            x,
            y,
            entries,
            st.ndvi_fit,
            st.ndvi_interior,
            st.ndvi_water,
            s["reference"],
            s["feather_m"],
        )
    else:
        outs = _ndvi_child_outputs(_MARKERS.read, z, x, y, planned)
        inputs = inputs_hash(st.settings["ndvi"], __version__, "parent", *outs)
        produce = partial(_ndvi_parent_bytes, st.pkg, z, x, y)
    return run_tile(st.pkg, _MARKERS, "ndvi", "png", z, x, y, inputs, produce)


def ndvi_batch(items: list[tuple[int, int, int, bool, tuple[bool, ...]]]) -> list:
    return [_ndvi_tile(*it) for it in items]


# ---------------------------------------------------------------- build


def _chunks(items, n: int = CHUNK):
    buf = []
    for it in items:
        buf.append(it)
        if len(buf) == n:
            yield buf
            buf = []
    if buf:
        yield buf


def _terrain_items(plan: LayerPlan):
    for z in sorted(plan.tiles):
        x, y = split_keys(plan.tiles[z])
        for a, b in zip(x.tolist(), y.tolist()):
            yield (z, a, b)


def _imagery_phases(plan: LayerPlan):
    for z in range(plan.max_zoom, -1, -1):
        keys = plan.tiles[z]
        leaf = np.isin(keys, plan.leaves[z])
        x, y = split_keys(keys)
        yield _chunks((z, a, b, bool(lf)) for a, b, lf in zip(x.tolist(), y.tolist(), leaf.tolist()))


def _ndvi_phases(plan: LayerPlan):
    """_imagery_phases with each parent's planned-children flags (NDVI_KIDS order) as a fifth item field."""
    for z in range(plan.max_zoom, -1, -1):
        keys = plan.tiles[z]
        leaf = np.isin(keys, plan.leaves[z])
        x, y = split_keys(keys)
        yield _chunks(
            (z, a, b, bool(lf), () if lf else tuple(plan.has(z + 1, 2 * a + dx, 2 * b + dy) for dx, dy in NDVI_KIDS))
            for a, b, lf in zip(x.tolist(), y.tolist(), leaf.tolist())
        )


def _run_layer(
    ctx: BuildContext, layer: str, plan: LayerPlan, phases, fn, jobs: int, json_progress: bool, per_zoom: bool = False
) -> LayerStats:
    removed = remove_stale(Path(ctx.pkg), layer, EXT[layer], plan)
    if removed:
        log.info("%s: removed %d tiles that left the plan", layer, removed)
    stats = LayerStats(per_zoom={} if per_zoom else None)
    prog = Progress(layer, plan.count(), json_lines=json_progress)
    t0 = time.monotonic()

    def on_result(r):
        stats.add(r)
        prog.update(r)

    run_pool(fn, phases, jobs, init_worker, (ctx,), on_result)
    prog.done()
    stats.seconds = round(time.monotonic() - t0, 3)
    return stats


def _landcover_complete(index: dict, out: Path) -> bool:
    return all((out / t["file"]).is_file() for t in index["tiles"])


def _run_isolated(fn, *args):
    """fn(*args) in a fresh spawned process (fn must be importable; tests replace this with an inline call)."""
    with ProcessPoolExecutor(1, mp_context=multiprocessing.get_context("spawn")) as ex:
        return ex.submit(fn, *args).result()


def _cut_landcover(bounds: tuple, out: Path, paths: dict[str, Path]) -> tuple[dict, float]:
    """Runs in its own process, so its peak RSS isn't the main process's lifetime peak (fetch, planning)."""
    index = landcover.fetch(bounds, out, reader=landcover.local_reader(paths), cut_by="camsim-scene")
    return index, peak_rss_mb()


def _build_landcover(pkg: Path, m: Manifest, ctx: BuildContext) -> LayerStats:
    st, out = LayerStats(), pkg / "landcover"
    prio = m.layers["landcover"]["priorities"]
    if not prio:
        shutil.rmtree(out, ignore_errors=True)
        return st
    t0 = time.monotonic()
    rec = m.source(prio[0])
    paths = {a.url: Path(ctx.asset_paths[f"{rec.id}/{a.id}"]) for a in rec.assets}
    inputs = inputs_hash(m.layer_settings_hash("landcover"), __version__, *sorted(a.sha256 for a in rec.assets))
    marker, index_path = pkg / STATE_DIR / "landcover.json", out / "index.json"
    prev = json.loads(marker.read_text()) if marker.exists() else None
    index = None
    if prev and prev["inputs"] == inputs and index_path.exists() and sha256_file(index_path) == prev["output"]:
        index = json.loads(index_path.read_text())
        if not _landcover_complete(index, out):
            index = None
    if index is not None:
        st.skipped = len(index["tiles"])
    else:
        shutil.rmtree(out, ignore_errors=True)
        index, st.peak_rss_mb = _run_isolated(_cut_landcover, tuple(m.layers["landcover"]["bounds"]), out, paths)
        atomic_write(marker, json.dumps({"inputs": inputs, "output": sha256_file(index_path)}).encode())
        st.built = len(index["tiles"])
    st.tiles = len(index["tiles"])
    files = [p for p in out.iterdir() if p.is_file()]  # the PNGs + index.json + ATTRIBUTION.txt
    st.files, st.bytes = len(files), sum(p.stat().st_size for p in files)
    st.seconds = round(time.monotonic() - t0, 3)
    return st


def _fit_balance(pkg: Path, m: Manifest, ctx: BuildContext) -> dict | None:
    """Fit (or reuse) imagery/balance.json before the imagery tiles; the report goes to build.json."""
    s = m.layers["imagery"].get("balance")
    out = pkg / "imagery" / colour_balance.FILE
    marker = pkg / STATE_DIR / "balance.json"
    if not s:
        out.unlink(missing_ok=True)
        marker.unlink(missing_ok=True)
        return None
    classes, class_shas = class_rasters(m, ctx.asset_paths)
    if not classes:
        log.warning(
            "balance: no WorldCover in the package: NAIP is not clipped at the coast and the colour match also "
            "applies over water"
        )
    elif "naip_water_buffer_m" not in s or "water_fade_m" not in s:
        log.warning(
            "balance: the manifest has no water settings (planned before the water mask): NAIP is not clipped at "
            "the coast and the colour match applies over water; re-plan (build --replan) to enable the water mask"
        )
    shas = sorted(a.sha256 for sid in (s["reference"], s["target"]) for a in m.source(sid).assets) + sorted(class_shas)
    inputs = inputs_hash(m.layer_settings_hash("imagery"), __version__, "balance", *shas)

    def fit():
        fps = reference_footprints(m, s["reference"])
        bal = None
        if fps:
            st = WorkerState(ctx)
            bounds = shapely.union_all(fps).bounds
            bal = colour_balance.fit_balance(
                st.index["imagery"], classes, bounds, colour_balance.grid_for(m, s["cell_km"]), s
            )
        if bal is None:
            return None, {"fitted": False, "water_mask": False}
        return bal.to_json(), {
            "fitted": True,
            **bal.report,
            "water_mask": bool(classes) and bool(s.get("water_fade_m")),
        }

    report = _cached_fit(out, marker, inputs, fit)
    if not report["skipped"]:
        log.info("balance: %s in %.1f s", "fitted" if report["fitted"] else "not fitted", report["seconds"])
    return report


def _cached_fit(out: Path, marker: Path, inputs: str, fit) -> dict:
    """A fitted file (imagery/balance.json, ndvi/fit.json) and its .state marker: reused when the marker has the same
    inputs and the file still matches its output; else both are dropped and fit() -> (file bytes or None, report) runs,
    and the file (unless None) and the marker are written. Returns the report (+ "seconds", "skipped") for build.json."""
    prev = json.loads(marker.read_text()) if marker.exists() else None
    if prev and prev["inputs"] == inputs:
        have = sha256_file(out) if out.exists() else None
        if have == prev["output"]:
            return {**prev["report"], "skipped": True}
    t0 = time.monotonic()
    out.unlink(missing_ok=True)  # an old or unreadable file must never block the refit
    marker.unlink(missing_ok=True)
    data, report = fit()
    output = None
    if data is not None:
        atomic_write(out, data)
        output = sha256_file(out)
    report["seconds"] = round(time.monotonic() - t0, 3)
    atomic_write(marker, json.dumps({"inputs": inputs, "output": output, "report": report}).encode())
    return {**report, "skipped": False}


def _fit_ndvi(pkg: Path, m: Manifest, ctx: BuildContext) -> dict | None:
    """Fit (or reuse) ndvi/fit.json before the NDVI tiles; the report goes to build.json. None without an NDVI layer
    that has both NAIP and Sentinel-2."""
    s = m.layers.get("ndvi")
    out = pkg / "ndvi" / ndvi_fit.FILE
    marker = pkg / STATE_DIR / "ndvi_fit.json"
    if not s or not s["reference"] or not s["target"]:
        out.unlink(missing_ok=True)
        marker.unlink(missing_ok=True)
        return None
    if "cell_km" not in s:
        raise BuildError("ndvi: manifest.json was planned before the gridded NDVI fit; re-plan it (build --replan)")
    classes, class_shas = class_rasters(m, ctx.asset_paths)
    shas = sorted(a.sha256 for sid in (s["reference"], s["target"]) for a in m.source(sid).assets) + sorted(class_shas)
    fmt = f"format{ndvi_fit.FORMAT}"  # a new file format refits even with unchanged settings
    inputs = inputs_hash(m.layer_settings_hash("ndvi"), __version__, "ndvi_fit", fmt, *shas)

    def fit():
        fps = reference_footprints(m, s["reference"])
        f = None
        if fps:
            st = WorkerState(ctx)
            dates = {
                f"{s['reference']}/{a.id}": a.metadata.get("acquired", "") for a in m.source(s["reference"]).assets
            }
            f = ndvi_fit.fit_ndvi(st.index["ndvi"], classes, shapely.union_all(fps).bounds, s, dates)
        if f is None:
            log.warning("ndvi: no fit; NAIP NDVI stays on its own (DN) scale")
            return None, {"fitted": False}
        return f.to_json(), {"fitted": True, **f.report}

    return _cached_fit(out, marker, inputs, fit)


def _ndvi_area(m: Manifest, use_bbox: bool = False):
    """Where the NDVI layer is built: the ring, within the footprints of its sources' data."""
    ring = next(r for r in m.region_objs() if r.name == "ring").geometry()
    return shapely.intersection(ring, shapely.union_all(coverage(m, "ndvi", use_bbox=use_bbox).geoms))


def _build_ndvi(pkg: Path, m: Manifest, ctx: BuildContext, iplan: LayerPlan, jobs: int, json_progress: bool):
    """The NDVI tiles and their tilemapresource.xml (zooms and bounds of the tiles written); removes the layer when
    the manifest has none. Returns its stats, or None."""
    s = m.layers.get("ndvi")
    if not s:
        shutil.rmtree(pkg / "ndvi", ignore_errors=True)
        shutil.rmtree(pkg / STATE_DIR / "ndvi", ignore_errors=True)
        return None
    plan = ndvi_plan(iplan, s["max_zoom"], _ndvi_area(m))
    phases = _ndvi_phases(plan) if plan.count() else []
    stats = _run_layer(ctx, "ndvi", plan, phases, ndvi_batch, jobs, json_progress, per_zoom=True)
    xml = pkg / "ndvi" / "tilemapresource.xml"
    present = {z: k for z, k in present_tiles(pkg, "ndvi", "png").items() if len(k)}
    if present:
        bounds = plan_bounds(LayerPlan(present, {}))
        atomic_write(xml, tilemapresource_xml(m.name, max(present), bounds, "image/png", "png").encode())
    else:
        xml.unlink(missing_ok=True)
    return stats


def _git_commit() -> str | None:
    if os.environ.get("CAMSIM_SCENE_GIT_COMMIT"):
        return os.environ["CAMSIM_SCENE_GIT_COMMIT"]
    try:
        r = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=Path(__file__).parent, capture_output=True, text=True, check=True
        )
        return r.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def _credits(m: Manifest, layer: str) -> str:
    return "; ".join(m.source(s).attribution for s in m.layers[layer]["priorities"])


def build_scene(
    pkg: Path,
    cache: Cache,
    jobs: int | None = None,
    json_progress: bool = False,
    max_worker_rss_mb: float = 2048.0,
    locked: bool = False,
) -> dict:
    """Fetch what's missing, then build. `locked`: the caller already holds the package's build lock."""
    pkg = Path(pkg)
    jobs = jobs or os.cpu_count() or 1
    started = _utc_now()
    with _lock(pkg, locked):  # before the fetch too: a second build must not rewrite manifest.json
        m = Manifest.load(pkg / "manifest.json")
        if needs_fetch(m):
            m = fetch_scene(pkg, cache, locked=True)
        ctx = make_context(pkg, m, cache)
        tplan = plan_tiles(m.region_objs(), "terrain", coverage(m, "terrain"))
        iplan = plan_tiles(m.region_objs(), "imagery", coverage(m, "imagery"))
        bal_report = _fit_balance(pkg, m, ctx)  # before any worker starts: they load balance.json
        ndvi_report = _fit_ndvi(pkg, m, ctx)  # before any worker starts: they load fit.json
        stats = {
            "terrain": _run_layer(
                ctx, "terrain", tplan, [_chunks(_terrain_items(tplan))], terrain_batch, jobs, json_progress
            )
        }
        lj = layer_json(m.name, available_ranges(tplan), _credits(m, "terrain"))
        atomic_write(pkg / "terrain" / "layer.json", canonical_json(lj).encode())
        stats["imagery"] = _run_layer(ctx, "imagery", iplan, _imagery_phases(iplan), imagery_batch, jobs, json_progress)
        atomic_write(
            pkg / "imagery" / "tilemapresource.xml",
            tilemapresource_xml(m.name, iplan.max_zoom, plan_bounds(iplan)).encode(),
        )
        ndvi_stats = _build_ndvi(pkg, m, ctx, iplan, jobs, json_progress)
        if ndvi_stats is not None:
            stats["ndvi"] = ndvi_stats
        stats["landcover"] = _build_landcover(pkg, m, ctx)
        atomic_write(pkg / "ATTRIBUTION.txt", attribution_text(m).encode())
        normalise_modes(pkg, skip=(STATE_DIR,))
        sea = sea_level_mod.write(pkg, m, {k: Path(v) for k, v in ctx.grid_paths.items()})
        if sea:
            log.info("sea level: EGM96 %+.3f m (NOAA %s %s)", sea["offset_m"], sea["station"], sea["name"])
        m.hashes_sha256 = write_hashes(pkg, Markers(pkg).output_for_file)
        m.write(pkg / "manifest.json")
        info = {
            "started_utc": started,
            "finished_utc": _utc_now(),
            "host": platform.node(),
            "platform": platform.platform(),
            "python": platform.python_version(),
            "tool_version": __version__,
            "git_commit": _git_commit(),
            "image_digest": os.environ.get("CAMSIM_SCENE_IMAGE_DIGEST"),
            "jobs": jobs,
            "cpu_count": os.cpu_count(),
            "layers": {k: v.to_dict() for k, v in stats.items()},
            "balance": bal_report,
            "ndvi": ndvi_report,
        }
        atomic_write(pkg / "build.json", (json.dumps(info, indent=2, sort_keys=True) + "\n").encode())
    peak = max(s.peak_rss_mb for s in stats.values())
    if peak > max_worker_rss_mb:
        raise BuildError(
            f"peak worker RSS {peak:.0f} MB exceeds {max_worker_rss_mb:.0f} MB (the package is complete; see build.json)"
        )
    return info
