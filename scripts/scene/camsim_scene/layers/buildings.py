"""Buildings layer build (REALISM R3, R3b): snapshot -> plan_buildings (heights from the package's own bare-earth
terrain sources, the same height path verify --deep uses) -> one GLB per tile + tileset.json under features/buildings/,
plus the footprint mask (features/buildings/mask/) from the shared footprint raster. The plan and the raster are cached
under .state/ by their inputs, so the land-cover burn and this layer read the same buildings and plan them once.
Rebuilt as a whole (Pendleton ~82k buildings) and skipped when its inputs hash is unchanged."""

from __future__ import annotations

import json
import logging
import pickle
import shutil
import time
from pathlib import Path

import numpy as np

from .. import __version__
from .. import buildings as B
from .. import footprints as F
from ..context import BuildContext, WorkerState
from ..engine import LayerStats, inputs_hash, peak_rss_mb
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
PLAN_MARKER, PLAN_FILE = "buildings-plan.json", "buildings-plan.pkl"
RASTER_DIR = "buildings-footprints"  # footprints.save_raster directory
LEGACY_STATE = ("buildings-footprints.json", "buildings-footprints.npz")  # the pre-chunk single-file raster


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


def _cache_hit(pkg: Path, marker: str, data: str, inputs: str) -> Path | None:
    mk, path = Path(pkg) / STATE_DIR / marker, Path(pkg) / STATE_DIR / data
    if mk.exists() and path.exists() and json.loads(mk.read_text()).get("inputs") == inputs:
        return path
    return None


def plan_cached(pkg: Path, m: Manifest, ctx: BuildContext, wst: WorkerState | None = None) -> tuple[list, dict]:
    """plan_from_manifest, pickled under .state/ by footprint_inputs (the land-cover burn and the GLBs share it)."""
    wst = wst or WorkerState(ctx)
    key = footprint_inputs(m, wst)
    hit = _cache_hit(pkg, PLAN_MARKER, PLAN_FILE, key)
    if hit is not None:
        try:
            return pickle.loads(hit.read_bytes())
        except Exception as e:  # noqa: BLE001 - a truncated or stale pickle: plan again
            log.warning("ignoring %s: %s", hit, e)
    out = plan_from_manifest(m, ctx, wst)
    atomic_write(Path(pkg) / STATE_DIR / PLAN_FILE, pickle.dumps(out, protocol=pickle.HIGHEST_PROTOCOL))
    atomic_write(Path(pkg) / STATE_DIR / PLAN_MARKER, json.dumps({"inputs": key}).encode())
    return out


def footprint_raster(pkg: Path, m: Manifest, ctx: BuildContext, wst: WorkerState | None = None,
                     jobs: int = 1) -> tuple[Path, str]:
    """The cached footprint raster directory of the kept buildings (footprints.save_raster) and its inputs key."""
    wst = wst or WorkerState(ctx)
    key = inputs_hash(footprint_inputs(m, wst), F.MASK_RULES_VERSION)
    out = Path(pkg) / STATE_DIR / RASTER_DIR
    for name in LEGACY_STATE:
        (Path(pkg) / STATE_DIR / name).unlink(missing_ok=True)
    if F.raster_key(out) != key:
        blds, _ = plan_cached(pkg, m, ctx, wst)
        F.save_raster(F.raster_tiles(blds), out, key)
    return out, key


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


def plan_stats(pkg: Path) -> dict | None:
    marker = Path(pkg) / STATE_DIR / MARKER
    return json.loads(marker.read_text())["stats"] if marker.exists() else None


def build(pkg: Path, m: Manifest, ctx: BuildContext) -> LayerStats:
    st, out = LayerStats(), Path(pkg) / OUT
    s = m.layers.get("buildings")
    if not s:
        shutil.rmtree(Path(pkg) / "features", ignore_errors=True)
        for name in (MARKER, PLAN_MARKER, PLAN_FILE, *LEGACY_STATE):
            (Path(pkg) / STATE_DIR / name).unlink(missing_ok=True)
        shutil.rmtree(Path(pkg) / STATE_DIR / RASTER_DIR, ignore_errors=True)
        return st
    t0 = time.monotonic()
    rec = m.source(s["priorities"][0])
    wst = WorkerState(ctx)
    credit = layer_credit(rec.attribution, rec.licence, layer="buildings")
    inputs = inputs_hash(m.layer_settings_hash("buildings"), __version__, rec.assets[0].sha256, credit,
                         footprint_inputs(m, wst))
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
    blds, stats = plan_cached(pkg, m, ctx, wst)
    by: dict[tuple[int, int, int], list] = {}
    for b in blds:
        by.setdefault(b.tile, []).append(b)
    # tiles only exist for keys with at least one building: write_glb rejects empty meshes
    groups = dict(sorted(by.items()))
    tiles = {k: B.tile_summary(k, v) for k, v in groups.items()}
    sampler = ImageryRoofSampler(wst) if roof is not None else None
    roof_stats: dict = {}
    for (z, x, y), bl in groups.items():
        mesh, table = B.tile_mesh(bl, tiles[(z, x, y)][1], roof, sampler, roof_stats)
        p = out / str(z) / str(x) / f"{y}.glb"
        p.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(p, write_glb(mesh, table, credit.replace("\n", " ")))
    if roof is not None:  # stats is the cached plan's dict: copy it, never mutate it
        stats = {**stats, "roofs": {"textured": 0, "flat_coarse": 0, "flat_overflow": 0, "tiles_by_texel_m": {},
                                    **roof_stats}}
    output = mask_sha = ""  # no tileset and no mask when nothing survived planning
    if tiles:
        out.mkdir(parents=True, exist_ok=True)
        atomic_write(ts_path, canonical_json(B.tileset_json(tiles)).encode())
        output = sha256_file(ts_path)
        if s.get("mask"):  # R3b; an R3 manifest (no "mask" settings) builds exactly as before
            raster_path, _ = footprint_raster(pkg, m, ctx, wst)
            mask_sha = write_mask(pkg, F.FootprintRaster(raster_path))
    atomic_write(marker, json.dumps({"inputs": inputs, "output": output, "mask": mask_sha, "stats": stats}).encode())
    files = [p for p in out.rglob("*") if p.is_file()]
    st.tiles = st.built = len(tiles)
    st.files, st.bytes = len(files), sum(p.stat().st_size for p in files)
    st.peak_rss_mb = peak_rss_mb()
    st.seconds = round(time.monotonic() - t0, 3)
    return st
