"""Buildings layer build (REALISM R3): snapshot -> plan_buildings (heights from the package's own bare-earth terrain
sources, the same height path verify --deep uses) -> one GLB per tile + tileset.json under features/buildings/.
Rebuilt as a whole (it is small: Pendleton ~82k buildings) and skipped when its inputs hash is unchanged."""

from __future__ import annotations

import json
import shutil
import time
from pathlib import Path

import numpy as np

from .. import __version__
from .. import buildings as B
from ..context import BuildContext, WorkerState
from ..engine import LayerStats, inputs_hash, peak_rss_mb
from ..fsutil import atomic_write, sha256_file
from ..gltf import write_glb
from ..licences import layer_credit
from ..manifest import STATE_DIR, Manifest, canonical_json
from . import terrain

OUT = Path("features") / "buildings"


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


def plan_from_manifest(m: Manifest, ctx: BuildContext, st: WorkerState | None = None):
    s = m.layers["buildings"]
    rec = m.source(s["priorities"][0])
    snapshot = Path(ctx.asset_paths[f"{rec.id}/{rec.assets[0].id}"])
    st = st or WorkerState(ctx)
    return B.plan_buildings(B.read_snapshot(snapshot), heights_fn(st, m), float(s["min_area_m2"]))


def plan_stats(pkg: Path) -> dict | None:
    marker = Path(pkg) / STATE_DIR / "buildings.json"
    return json.loads(marker.read_text())["stats"] if marker.exists() else None


def build(pkg: Path, m: Manifest, ctx: BuildContext) -> LayerStats:
    st, out = LayerStats(), Path(pkg) / OUT
    s = m.layers.get("buildings")
    if not s:
        shutil.rmtree(Path(pkg) / "features", ignore_errors=True)
        (Path(pkg) / STATE_DIR / "buildings.json").unlink(missing_ok=True)
        return st
    t0 = time.monotonic()
    rec = m.source(s["priorities"][0])
    wst = WorkerState(ctx)
    terrain_shas = [a.sha256 for sid in m.layers["terrain"]["priorities"] for a in m.source(sid).assets]  # in order
    credit = layer_credit(rec.attribution, rec.licence, layer="buildings")
    inputs = inputs_hash(
        m.layer_settings_hash("buildings"), __version__, rec.assets[0].sha256, credit,
        m.layer_settings_hash("terrain"), *wst.terrain_extra, *terrain_shas,
    )
    marker, ts_path = Path(pkg) / STATE_DIR / "buildings.json", out / "tileset.json"
    prev = json.loads(marker.read_text()) if marker.exists() else None
    if prev and prev["inputs"] == inputs and not prev["output"] and not out.exists():
        st.seconds = round(time.monotonic() - t0, 3)  # no building survived planning: nothing on disk
        return st
    if prev and prev["inputs"] == inputs and prev["output"] and ts_path.exists() and sha256_file(ts_path) == prev["output"]:
        uris = [u for u, _ in B.walk_tileset(json.loads(ts_path.read_text()))]
        if all((out / u).is_file() for u in uris):
            st.tiles = st.skipped = len(uris)
            st.files = len(uris) + 1
            st.bytes = sum((out / u).stat().st_size for u in uris) + ts_path.stat().st_size
            st.seconds = round(time.monotonic() - t0, 3)
            return st
    shutil.rmtree(out, ignore_errors=True)
    blds, stats = plan_from_manifest(m, ctx, wst)
    by: dict[tuple[int, int, int], list] = {}
    for b in blds:
        by.setdefault(b.tile, []).append(b)
    # tiles only exist for keys with at least one building: write_glb rejects empty meshes
    tiles = {k: (v, B.tile_origin(*k, v)) for k, v in sorted(by.items())}
    for (z, x, y), (bl, origin) in tiles.items():
        mesh, table = B.tile_mesh(bl, origin)
        p = out / str(z) / str(x) / f"{y}.glb"
        p.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(p, write_glb(mesh, table, credit.replace("\n", " ")))
    output = ""  # no tileset when nothing survived planning
    if tiles:
        out.mkdir(parents=True, exist_ok=True)
        atomic_write(ts_path, canonical_json(B.tileset_json(tiles)).encode())
        output = sha256_file(ts_path)
    atomic_write(marker, json.dumps({"inputs": inputs, "output": output, "stats": stats}).encode())
    files = [p for p in out.rglob("*") if p.is_file()]
    st.tiles = st.built = len(tiles)
    st.files, st.bytes = len(files), sum(p.stat().st_size for p in files)
    st.peak_rss_mb = peak_rss_mb()
    st.seconds = round(time.monotonic() - t0, 3)
    return st
