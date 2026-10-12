"""Canopy cover and height tiles (REALISM R4, camsim-canopy-1) from CHMv2: two tile sets on the land-cover grid (0.05
degree, 600 x 600 8-bit greyscale PNG, row 0 north, same file names), canopy/cover/ (percent of CHM pixels >= 3 m) and
canopy/height/ (nearest-rank p90 of those pixels, metres), each with an index.json, over the package bbox. Per texel,
every CHM pixel whose centre falls in it counts (pixel centres reprojected EPSG:3857 -> lon/lat). A texel no CHM pixel
covers is no data: cover NO_DATA (255), height 0 (CamSim then uses the land-cover rule there, as outside the layer).
Skipped when its inputs are unchanged (.state/canopy.json)."""

from __future__ import annotations

import json
import shutil
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

from .. import __version__
from ..engine import LayerStats, inputs_hash, peak_rss_mb
from ..fsutil import atomic_write, sha256_file
from ..licences import layer_credit
from ..manifest import STATE_DIR, Manifest
from ..sources.worldcover import CELLS_PER_DEG, TILE_DEG, TILE_PX
from . import landcover

FORMAT = "camsim-canopy-1"
BANDS = {"cover": "percent", "height": "m"}
MIN_HEIGHT_M = 3
NO_DATA = 255  # cover value of a texel no CHM pixel covers
P90 = 0.9
R = 6378137.0
MARKER = "canopy.json"
OUT = Path("canopy")


def _merc(lon: float, lat: float) -> tuple[float, float]:
    return float(np.radians(lon) * R), float(R * np.log(np.tan(np.pi / 4 + np.radians(lat) / 2)))


def _stats(idx: np.ndarray, h: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """(cover %, p90 height) per texel from texel indices (0..TILE_PX^2-1) and uint8 heights of the pixels inside."""
    n = TILE_PX * TILE_PX
    total = np.bincount(idx, minlength=n)
    tall = h >= MIN_HEIGHT_M
    ti = idx[tall]
    ntall = np.bincount(ti, minlength=n)
    pct = np.zeros(n, np.float64)
    np.divide(100.0 * ntall, total, out=pct, where=total > 0)
    cover = np.floor(pct + 0.5).astype(np.uint8)
    cover[total == 0] = NO_DATA
    height = np.zeros(n, np.uint8)
    if ti.size:
        key = np.sort(ti.astype(np.int64) * 256 + h[tall].astype(np.int64))   # by texel, then height
        tex = key >> 8
        starts = np.flatnonzero(np.r_[True, tex[1:] != tex[:-1]])
        counts = np.diff(np.r_[starts, key.size])
        pick = starts + np.floor(P90 * (counts - 1)).astype(np.int64)
        height[tex[starts]] = (key[pick] & 255).astype(np.uint8)
    return cover.reshape(TILE_PX, TILE_PX), height.reshape(TILE_PX, TILE_PX)


def texel_stats(lon, lat, h, i: int, j: int) -> tuple[np.ndarray, np.ndarray]:
    """Cover/height of tile (i, j) from pixel centres given as lon/lat points (any shape, flattened)."""
    lon, lat, h = np.ravel(lon), np.ravel(lat), np.ravel(h).astype(np.uint8)
    col = np.floor((lon - j * TILE_DEG) * CELLS_PER_DEG).astype(np.int64)
    row = np.floor(((i + 1) * TILE_DEG - lat) * CELLS_PER_DEG).astype(np.int64)
    ok = (col >= 0) & (col < TILE_PX) & (row >= 0) & (row < TILE_PX)
    return _stats((row[ok] * TILE_PX + col[ok]).astype(np.int32), h[ok])


def tile_from_chm(i: int, j: int, chm_paths) -> tuple[np.ndarray, np.ndarray]:
    """Cover/height of tile (i, j) from the CHM files overlapping it. Texel indices are computed per CHM row and column
    (1-D) and combined as int32, so a 0.05-degree tile (~26 M pixels at 1.2 m) stays within a few hundred MB."""
    import rasterio
    from rasterio.errors import WindowError
    from rasterio.windows import Window, from_bounds

    x0, y0 = _merc(j * TILE_DEG, i * TILE_DEG)
    x1, y1 = _merc((j + 1) * TILE_DEG, (i + 1) * TILE_DEG)
    idx_parts, h_parts = [], []
    for p in chm_paths:
        with rasterio.open(p) as ds:
            b = ds.bounds
            if b.right <= x0 or b.left >= x1 or b.top <= y0 or b.bottom >= y1:
                continue
            try:
                win = from_bounds(x0, y0, x1, y1, ds.transform).round_offsets().round_lengths()
                win = win.intersection(Window(0, 0, ds.width, ds.height))
            except WindowError:
                continue
            a = ds.read(1, window=win)
            t = ds.window_transform(win)
        cx = t.c + (np.arange(a.shape[1]) + 0.5) * t.a
        cy = t.f + (np.arange(a.shape[0]) + 0.5) * t.e
        lon = np.degrees(cx / R)
        lat = np.degrees(2.0 * np.arctan(np.exp(cy / R)) - np.pi / 2.0)
        col = np.floor((lon - j * TILE_DEG) * CELLS_PER_DEG).astype(np.int64)
        row = np.floor(((i + 1) * TILE_DEG - lat) * CELLS_PER_DEG).astype(np.int64)
        cok, rok = (col >= 0) & (col < TILE_PX), (row >= 0) & (row < TILE_PX)
        if not cok.any() or not rok.any():
            continue
        idx = (row[rok].astype(np.int32)[:, None] * TILE_PX + col[cok].astype(np.int32)[None, :]).ravel()
        idx_parts.append(idx)
        h_parts.append(a[np.ix_(rok, cok)].ravel())
    if not idx_parts:
        return np.full((TILE_PX, TILE_PX), NO_DATA, np.uint8), np.zeros((TILE_PX, TILE_PX), np.uint8)
    return _stats(np.concatenate(idx_parts), np.concatenate(h_parts))


def _tile_job(args) -> tuple[int, int]:
    i, j, paths, out = args
    cover, height = tile_from_chm(i, j, paths)
    name = landcover.tile_filename(i, j)
    landcover.write_tile(cover, Path(out) / "cover" / name)
    landcover.write_tile(height, Path(out) / "height" / name)
    return i, j


def _index(rec, bounds, tiles, band: str, credit: str) -> dict:
    idx = landcover.build_index(bounds, tiles, [])
    idx.update(format=FORMAT, band=band, units=BANDS[band], source=rec.dataset, source_url=rec.assets[0].url if rec.assets else "",
               licence=rec.licence, attribution=rec.attribution, min_height_m=MIN_HEIGHT_M, height_stat="p90 nearest-rank")
    idx["acquired"] = sorted({d for a in rec.assets for d in a.metadata.get("acquired", [])})
    del idx["missing"]
    return idx


def _complete(out: Path, marker: dict) -> bool:
    for band in BANDS:
        p = out / band / "index.json"
        if not p.exists() or sha256_file(p) != marker["output"].get(band):
            return False
        if not all((out / band / t["file"]).is_file() for t in json.loads(p.read_text())["tiles"]):
            return False
    return True


def build(pkg: Path, m: Manifest, ctx, jobs: int = 1) -> LayerStats:
    st, out = LayerStats(), Path(pkg) / OUT
    s = m.layers.get("canopy")
    marker_path = Path(pkg) / STATE_DIR / MARKER
    if not s or not s.get("priorities"):
        shutil.rmtree(out, ignore_errors=True)
        marker_path.unlink(missing_ok=True)
        return st
    t0 = time.monotonic()
    rec = m.source(s["priorities"][0])
    credit = layer_credit(rec.attribution, rec.licence, layer="canopy")
    inputs = inputs_hash(m.layer_settings_hash("canopy"), __version__, credit, *sorted(a.sha256 or "" for a in rec.assets))
    tiles = landcover.tiles_for_bbox(*s["bounds"])
    prev = json.loads(marker_path.read_text()) if marker_path.exists() else None
    if prev and prev["inputs"] == inputs and _complete(out, prev):
        st.tiles = st.skipped = len(tiles)
    else:
        shutil.rmtree(out, ignore_errors=True)
        for band in BANDS:
            (out / band).mkdir(parents=True, exist_ok=True)
        paths = [ctx.asset_paths[f"{rec.id}/{a.id}"] for a in rec.assets if a.role == "data"]
        work = [(i, j, paths, str(out)) for i, j in tiles]
        if jobs > 1:
            with ProcessPoolExecutor(max_workers=jobs) as ex:
                list(ex.map(_tile_job, work))
        else:
            for w in work:
                _tile_job(w)
        output = {}
        for band in BANDS:
            atomic_write(out / band / "index.json",
                         (json.dumps(_index(rec, s["bounds"], tiles, band, credit), indent=2, ensure_ascii=False) + "\n").encode())
            output[band] = sha256_file(out / band / "index.json")
        atomic_write(out / "ATTRIBUTION.txt", (credit + "\n").encode())
        atomic_write(marker_path, json.dumps({"inputs": inputs, "output": output}).encode())
        st.tiles = st.built = len(tiles)
        st.peak_rss_mb = peak_rss_mb()
    files = [p for p in out.rglob("*") if p.is_file()]
    st.files, st.bytes = len(files), sum(p.stat().st_size for p in files)
    st.seconds = round(time.monotonic() - t0, 3)
    return st
