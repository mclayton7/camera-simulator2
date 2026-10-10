"""verify: quick checks always (hashes, availability, licences, attribution). --deep decodes a 2 % sample of the
terrain tiles per zoom (--all: every tile), compares vertex heights with the sources resampled independently
(highest-priority source, exact datum offsets, feather bands excluded; scene zooms and the global base have
separate limits), checks shared edges, NaNs and normals,
decodes every JPEG, and checks a sample of NDVI leaf pixels against the sources (`ndvi_values`). Writes
<pkg>.verify.json beside the package; the package itself is never modified."""

from __future__ import annotations

import io
import json
import math
import random
from concurrent.futures import ProcessPoolExecutor
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
from PIL import Image

from .cache import CacheError
from .config import TERRAIN_GRID as GRID
from .fsutil import atomic_write, sha256_file
from .licences import LicenceError, attribution_text, check_allowed
from .manifest import Manifest, package_files
from .net import HttpError
from .qmesh import QMAX, decode
from .sources.base import M_PER_DEG
from .tiling import LayerPlan, available_keys, make_keys, split_keys, tile_bounds
from .tms import parse_tilemapresource, plan_bounds

P50_M, P99_M = 0.05, 0.5  # scene zooms (above the global base)
# Global base zooms: the build applies vertical datum offsets through a lattice (exact every tile/32, bilinear in
# between) that verify evaluates exactly; at coarse zooms that difference is metres, so the base is held to 1 % of
# the zoom's TIN tolerance (z8 0.75 m, z3 24 m), never tighter than P99_M. A mirrored or offset tile is far above it.
BASE_TIN_FRACTION = 0.01
SAMPLE = 0.02
NDVI_PX = 64  # pixels checked per sampled NDVI leaf
NDVI_P99_STEPS = 1


def base_max_zoom(m: Manifest) -> int:
    """Highest zoom of the global base (the "globe" region's terrain max zoom), -1 when the package has none."""
    return max((int(r["max_zoom"].get("terrain", -1)) for r in m.regions if r["name"] == "globe"), default=-1)


def base_p99_limit(z: int) -> float:
    from .layers.terrain import max_error

    return max(P99_M, BASE_TIN_FRACTION * max_error(z))


@dataclass
class Check:
    name: str
    ok: bool
    detail: str = ""


def _guard(name: str, fn, *args) -> Check:
    """Run one check; a malformed package (missing file, bad line) fails the check instead of crashing verify."""
    try:
        return fn(*args)
    except Exception as e:  # noqa: BLE001 - any failure to check is a failed check
        return Check(name, False, f"{type(e).__name__}: {e}")


def _error_stats(e: np.ndarray) -> dict:
    return {
        "n": int(e.size),
        "p50": float(np.percentile(e, 50)) if e.size else 0.0,
        "p99": float(np.percentile(e, 99)) if e.size else 0.0,
        "max": float(e.max()) if e.size else 0.0,
    }


def _summary(problems: list[str]) -> str:
    more = f" (+{len(problems) - 10} more)" if len(problems) > 10 else ""
    return "; ".join(problems[:10]) + more


def present_tiles(pkg: Path, layer: str, ext: str) -> dict[int, np.ndarray]:
    out: dict[int, list[int]] = {}
    root = Path(pkg) / layer
    for zdir in root.iterdir() if root.exists() else []:
        if not (zdir.is_dir() and zdir.name.isdigit()):
            continue
        keys = out.setdefault(int(zdir.name), [])
        for xdir in zdir.iterdir():
            for f in xdir.iterdir() if xdir.is_dir() else []:
                if f.name.endswith(f".{ext}") and f.name[: -len(ext) - 1].isdigit():
                    keys.append((int(xdir.name) << 32) | int(f.name[: -len(ext) - 1]))
    return {z: np.unique(np.asarray(k, np.int64)) for z, k in sorted(out.items())}


def _check_hashes(pkg: Path, m: Manifest) -> Check:
    path = pkg / "hashes.txt"
    if not path.exists():
        return Check("hashes", False, "no hashes.txt")
    problems = []
    with open(path, encoding="utf-8") as f:
        listed = (line.rstrip("\n").split("  ", 1)[::-1] for line in f)
        files = iter(package_files(pkg))
        a, b = next(files, None), next(listed, None)
        while a is not None or b is not None:
            if b is None or (a is not None and a < b[0]):
                problems.append(f"unlisted {a}")
                a = next(files, None)
            elif a is None or b[0] < a:
                problems.append(f"missing {b[0]}")
                b = next(listed, None)
            else:
                if sha256_file(pkg / a) != b[1]:
                    problems.append(f"modified {a}")
                a, b = next(files, None), next(listed, None)
    if m.hashes_sha256 != sha256_file(path):
        problems.append("hashes.txt does not match manifest.hashes_sha256")
    return Check("hashes", not problems, _summary(problems))


def _check_available(pkg: Path) -> Check:
    lj = json.loads((pkg / "terrain" / "layer.json").read_text())
    want, have = available_keys(lj["available"]), present_tiles(pkg, "terrain", "terrain")
    bad = [
        f"z{z}: layer.json {len(want.get(z, []))} tiles, files {len(have.get(z, []))}"
        for z in sorted(set(want) | set(have))
        if not np.array_equal(want.get(z, np.zeros(0, np.int64)), have.get(z, np.zeros(0, np.int64)))
    ]
    return Check("terrain_available", not bad, _summary(bad))


def _check_tms(pkg: Path, layer: str = "imagery", ext: str = "jpg", name: str = "tilemapresource") -> Check:
    levels, bounds = parse_tilemapresource((pkg / layer / "tilemapresource.xml").read_text())
    have = {z: k for z, k in present_tiles(pkg, layer, ext).items() if len(k)}
    problems = []
    if levels != list(range(max(have) + 1)):
        problems.append(f"levels {levels[:3]}..{levels[-1:]} but tiles to z{max(have)}")
    hb = plan_bounds(LayerPlan(have, {}))
    if any(abs(a - b) > 1e-9 for a, b in zip(bounds, hb)):
        problems.append(f"BoundingBox {bounds} != tiles {hb}")
    return Check(name, not problems, _summary(problems))


def _check_licences(m: Manifest) -> Check:
    try:
        check_allowed({s.id: s.licence for s in m.sources}, m.licence_allow)
        return Check("licences", True)
    except LicenceError as e:
        return Check("licences", False, str(e))


def _check_attribution(pkg: Path, m: Manifest) -> Check:
    p = pkg / "ATTRIBUTION.txt"
    ok = p.exists() and p.read_text(encoding="utf-8") == attribution_text(m)
    return Check("attribution", ok, "" if ok else "ATTRIBUTION.txt differs from the text generated from manifest.json")


def _edge_problems(qa, qb, a_edge, b_edge, along_a, along_b) -> int:
    ha = dict(zip(along_a[a_edge].tolist(), qa.heights()[a_edge]))
    hb = dict(zip(along_b[b_edge].tolist(), qb.heights()[b_edge]))
    step = max(qa.max_height - qa.min_height, qb.max_height - qb.min_height) / QMAX
    return sum(1 for k in set(ha) & set(hb) if abs(ha[k] - hb[k]) > step + 1e-6)


def _jpeg_bad(paths: list[str]) -> list[str]:
    bad = []
    for p in paths:
        try:
            im = Image.open(io.BytesIO(Path(p).read_bytes()))
            im.load()
            if im.size != (256, 256) or im.mode != "RGB":
                bad.append(f"{p}: {im.size} {im.mode}")
        except Exception as e:  # noqa: BLE001 - any decode failure is a finding
            bad.append(f"{p}: {e}")
    return bad


def _ndvi_leaves(present: dict[int, np.ndarray]) -> dict[int, np.ndarray]:
    """NDVI tiles with no tile below them (a parent is written only where a child holds data)."""
    out = {}
    for z, keys in present.items():
        kids = present.get(z + 1)
        if kids is None or not len(kids):
            out[z] = keys
            continue
        x, y = split_keys(kids)
        out[z] = keys[~np.isin(keys, np.unique(make_keys(x >> 1, y >> 1)))]
    return out


def _ndvi_values(pkg: Path, m: Manifest, st, rng: random.Random, all_tiles: bool) -> tuple[Check, dict]:
    """Sampled leaf pixels against NDVI recomputed from the sources at the pixel centre (the fit applied to the
    reference): pixels that are purely the reference (feather weight 1, or nothing behind it: "naip") or purely
    the rest ("s2"); feather pixels are left out. Each group must agree within NDVI_P99_STEPS at p99, and nodata
    must be nodata."""
    from .layers import imagery, ndvi

    s = m.layers["ndvi"]
    present = {z: k for z, k in present_tiles(pkg, "ndvi", "png").items() if len(k)}
    errs: dict[str, list[np.ndarray]] = {"naip": [], "s2": []}
    bad, leaves = [], 0
    for z, keys in sorted(_ndvi_leaves(present).items()):
        ks = keys.tolist()
        if not ks:
            continue
        for k in sorted(ks if all_tiles else rng.sample(ks, max(1, math.ceil(SAMPLE * len(ks))))):
            x, y = k >> 32, k & 0xFFFFFFFF
            try:
                code = ndvi.read_png((pkg / "ndvi" / str(z) / str(x) / f"{y}.png").read_bytes())
            except Exception as e:  # noqa: BLE001 - any decode failure is a finding
                bad.append(f"{z}/{x}/{y}: {e}")
                continue
            entries = st.index["ndvi"].query(imagery.query_bounds(z, x, y, s["feather_m"]))
            ref = [e for e in entries if imagery.source_of(e) == s["reference"]]
            rest = [e for e in entries if imagery.source_of(e) != s["reference"]]
            allow, dpx = ndvi.water_allow(z, x, y, st.ndvi_water, s["feather_m"])
            buffer_m = st.ndvi_water.buffer_m if st.ndvi_water is not None else 0.0
            wt = ndvi.reference_weight(z, x, y, ref, s["feather_m"], allow, dpx, buffer_m).ravel()
            (lon, lat), d = imagery.pixel_grid(z, x, y)
            idx = np.array(sorted(rng.sample(range(lon.size), NDVI_PX)))
            plon, plat, w, have = lon.ravel()[idx], lat.ravel()[idx], wt[idx], code.ravel()[idx]
            rn, rok, _ = ndvi.sample_ndvi(ref, plon, plat, d * M_PER_DEG)
            if st.ndvi_fit is not None:
                rn = st.ndvi_fit.apply(rn, plon, plat)
            sn, sok, _ = ndvi.sample_ndvi(rest, plon, plat, d * M_PER_DEG)
            use_ref = rok & ((w >= 1.0) | ~sok)
            use_rest = sok & (~rok | (w <= 0.0))
            want = np.where(use_ref, rn, np.where(use_rest, sn, np.nan))
            for name, pure in (("naip", use_ref), ("s2", use_rest)):
                errs[name].append(np.abs(have[pure].astype(np.int64) - ndvi.encode(want[pure]).astype(np.int64)))
            if (have[~(rok | sok)] != 0).any():
                bad.append(f"{z}/{x}/{y}: data where the sources have none")
            if (have[use_ref | use_rest] == 0).any():
                bad.append(f"{z}/{x}/{y}: nodata where the sources have data")
            leaves += 1
    parts = {k: np.concatenate(v).astype(np.float64) if v else np.zeros(0) for k, v in errs.items()}
    stats = {**_error_stats(np.concatenate(list(parts.values()))), "leaves": leaves}
    for k, e in parts.items():
        stats[k] = {"n": int(e.size), "p99": _error_stats(e)["p99"]}
    over = [f"{k} p99 {stats[k]['p99']:.1f} steps" for k in parts if stats[k]["p99"] > NDVI_P99_STEPS]
    ok = not bad and not over
    detail = (
        f"{stats['n']} pixels in {leaves} leaves: p99 {stats['p99']:.1f} steps, max {stats['max']:.0f}"
        f" (naip {stats['naip']['n']} px p99 {stats['naip']['p99']:.1f},"
        f" s2 {stats['s2']['n']} px p99 {stats['s2']['p99']:.1f})"
    )
    if over:
        detail += "; over " + ", ".join(over)
    if bad:
        detail += "; " + _summary(bad)
    return Check("ndvi_values", ok, detail), stats


def _deep(pkg: Path, m: Manifest, cache, all_tiles: bool, jobs: int) -> tuple[list[Check], dict, dict | None]:
    from .context import WorkerState
    from .layers import terrain
    from .pipeline import make_context

    st = WorkerState(make_context(pkg, m, cache))
    rng = random.Random(m.seed)
    present = present_tiles(pkg, "terrain", "terrain")
    errs: dict[int, list[np.ndarray]] = {}
    nonfinite, normals_bad, edges_bad = [], [], 0
    for z, keys in present.items():
        ks = keys.tolist()
        pick = ks if all_tiles else rng.sample(ks, max(1, math.ceil(SAMPLE * len(ks))))
        for k in sorted(pick):
            x, y = k >> 32, k & 0xFFFFFFFF
            q = decode((pkg / "terrain" / str(z) / str(x) / f"{y}.terrain").read_bytes())
            h = q.heights()
            lon, lat = q.lonlat(tile_bounds(z, x, y))
            if not np.isfinite(h).all():
                nonfinite.append(f"{z}/{x}/{y}")
                continue
            n = q.normals()
            if n is None or not (np.isfinite(n).all() and np.allclose(np.linalg.norm(n, axis=1), 1.0, atol=1e-3)):
                normals_bad.append(f"{z}/{x}/{y}")
            entries = st.index["terrain"].query(terrain.query_bounds(z, x, y))
            seam = terrain.tile_grid(z, x, y, entries).seam
            col = np.rint(q.u / QMAX * (GRID - 1)).astype(int)
            row = GRID - 1 - np.rint(q.v / QMAX * (GRID - 1)).astype(int)
            keep = ~seam[row, col]
            errs.setdefault(z, []).append(np.abs(h - terrain.point_heights(z, lon, lat, entries))[keep])
            for nx, ny, east in ((x + 1, y, True), (x, y + 1, False)):
                if not np.isin(make_keys(nx, ny), keys):
                    continue
                qb = decode((pkg / "terrain" / str(z) / str(nx) / f"{ny}.terrain").read_bytes())
                if east:
                    edges_bad += _edge_problems(q, qb, q.east, qb.west, q.v, qb.v)
                else:
                    edges_bad += _edge_problems(q, qb, q.north, qb.south, q.u, qb.u)
    per_zoom = {z: np.concatenate(v) for z, v in sorted(errs.items())}
    stats = _error_stats(np.concatenate(list(per_zoom.values())) if per_zoom else np.zeros(0))
    stats["by_zoom"] = {str(z): _error_stats(e) for z, e in per_zoom.items()}
    base = base_max_zoom(m)
    scene = [e for z, e in per_zoom.items() if z > base]
    stats["scene"] = _error_stats(np.concatenate(scene) if scene else np.zeros(0))
    stats["base_max_zoom"] = base
    over = []
    for z in per_zoom:
        if z <= base:
            zs = stats["by_zoom"][str(z)]
            zs["p99_limit"] = base_p99_limit(z)
            if zs["p99"] > zs["p99_limit"]:
                over.append(f"z{z} p99 {zs['p99']:.2f} m > {zs['p99_limit']:.2f} m")
    sc = stats["scene"]
    scene_ok = sc["p50"] <= P50_M and sc["p99"] <= P99_M
    worst = max(stats["by_zoom"].items(), key=lambda kv: kv[1]["p99"], default=None)
    heights_text = (
        f"scene z{base + 1}+: p50 {sc['p50']:.3f} m, p99 {sc['p99']:.3f} m, max {sc['max']:.2f} m over {sc['n']} vertices"
        + (f"; base z0-z{base}: " + (", ".join(over) if over else "within limits") if base >= 0 else "")
        + (f"; worst zoom z{worst[0]}: p99 {worst[1]['p99']:.3f} m, max {worst[1]['max']:.2f} m" if worst else "")
    )
    jpgs = [str(p) for p in sorted((pkg / "imagery").rglob("*.jpg"))]
    chunks = [jpgs[i : i + 512] for i in range(0, len(jpgs), 512)]
    if jobs > 1 and len(chunks) > 1:
        with ProcessPoolExecutor(jobs) as ex:
            bad_jpg = [b for part in ex.map(_jpeg_bad, chunks) for b in part]
    else:
        bad_jpg = [b for c in chunks for b in _jpeg_bad(c)]
    ndvi_stats = None
    ndvi_checks = []
    if m.layers.get("ndvi"):
        c, ndvi_stats = _ndvi_values(pkg, m, st, random.Random(m.seed + 1), all_tiles)
        ndvi_checks.append(c)
    checks = [
        Check(
            "terrain_heights",
            scene_ok and not over,
            heights_text,
        ),
        Check("terrain_finite", not nonfinite, _summary(nonfinite)),
        Check("terrain_normals", not normals_bad, _summary(normals_bad)),
        Check("terrain_edges", edges_bad == 0, f"{edges_bad} shared-edge vertices disagree" if edges_bad else ""),
        Check("imagery_decode", not bad_jpg, _summary(bad_jpg)),
    ]
    checks += ndvi_checks
    return checks, stats, ndvi_stats


def verify(
    pkg: Path, cache=None, deep: bool = False, all_tiles: bool = False, jobs: int = 1, report_path: Path | None = None
) -> dict:
    pkg = Path(pkg)
    m = Manifest.load(pkg / "manifest.json")
    checks = [
        _guard("hashes", _check_hashes, pkg, m),
        _guard("terrain_available", _check_available, pkg),
        _guard("tilemapresource", _check_tms, pkg),
        _guard("licences", _check_licences, m),
        _guard("attribution", _check_attribution, pkg, m),
    ]
    if {z: k for z, k in present_tiles(pkg, "ndvi", "png").items() if len(k)}:
        checks.append(_guard("ndvi_tilemapresource", _check_tms, pkg, "ndvi", "png", "ndvi_tilemapresource"))
    report: dict = {"package": str(pkg), "deep": deep}
    if deep:
        if cache is None:
            raise ValueError("verify --deep needs the fetch cache (the sources)")
        try:
            deep_checks, report["terrain_error_m"], ndvi_stats = _deep(pkg, m, cache, all_tiles, jobs)
            if ndvi_stats is not None:
                report["ndvi_error_steps"] = ndvi_stats
        except (CacheError, HttpError):
            raise  # the sources aren't available: an environment error, not a finding about the package
        except Exception as e:  # noqa: BLE001 - e.g. a truncated tile: the deep checks fail, the report is written
            deep_checks = [Check("deep", False, f"{type(e).__name__}: {e}")]
        checks += deep_checks
    report["checks"] = [asdict(c) for c in checks]
    report["ok"] = all(c.ok for c in checks)
    atomic_write(report_path or pkg.parent / f"{pkg.name}.verify.json", (json.dumps(report, indent=2) + "\n").encode())
    return report
