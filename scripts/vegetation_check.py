#!/usr/bin/env python3
# Copyright CamSim Contributors. All Rights Reserved.
"""Vegetation acceptance (REALISM R4 slice, docs/vegetation.md): placement on code 10, grounding, determinism from two
approach paths, origin-shift stability, off switch vs the pre-slice baseline, ground-truth check still passing.

  caffeinate -ims uv run --project scripts/scene --with pycocotools python scripts/vegetation_check.py \
      --scene "$PKG" --baseline .cache/veg/baseline --out .cache/veg/check

PKG is the scene package the baseline was captured with (Pendleton R2: .cache/scene-packages/pendleton-r2, absolute).
The baseline is the pre-slice `run_bench.py --smoke --site pendleton` pair (a/, b/ with shots/).

Gates (report.json, exit 0 only when all pass):
  placement     every dumped tree stands on a code whose species mix lists trees (and, inside the package's canopy layer,
                on a texel with cover > 0 and height >= 3 m); every shrub on a code whose mix lists shrubs
  species       per code, the dumped type counts vs the mix weights: chi-square p > 0.001 (REALISM R4)
  canopy        (packages with canopy/) per 100 m cell with mean CHMv2 cover in [0.1, 0.5]: crown fraction vs cover,
                median |error| <= 10 pp over >= 50 cells
  far_determinism  far blocks from the two approach runs have the same hashes; far trees are near trees of their cell
  handoff       events: no near/far switch leaves a block without actors (a destroy only after the other tier settled)
  long_range    (`--stages long_range`) the bench's long_range phase: 0 dropped frames, game p95 <= +3 ms over off,
                and >= 200 far trees drawn in its view (3D distance within their dissolve bucket's cull)
  r4_identity   (`--stages r4_identity`) --r4-scene with shrubs and far off: every cell has the --r4-baseline hash
  grounding     camsim.Vegetation.GroundCheck: n == 200 and max_dz_m < 0.5, both runs
  determinism   approach from 3 km north and 3 km south: every cell in both dumps has the same hash
  origin_shift  /snapshot at the riparian pose before and after a trip to lon + 3.41: SSIM >= baseline floor - 0.01,
                and every cell spawned again after the trip has its first hash
  off_switch    CAMSIM_VEGETATION_ENABLED=0 smoke shots vs the baseline: SSIM >= the baseline's own pair SSIM - 0.005, and
                the run's log has no vegetation line (a trees-on smoke is measured beside it: the one smoke shot, nadir
                from 3 km, barely sees trees, so this gate has little power)
  gt            scripts/gt_occlusion_check.py exits 0 (vegetation on)
  footprints    (`--stages footprints`, REALISM R3b; needs a package with `features/buildings/mask/`) every dumped tree's
                trunk and its 4 probes at `building_clearance_m` (E/W/N/S) are off the package's mask, no trunk lies
                inside a footprint polygon shrunk by half a mask cell's diagonal, and the log names the mask method;
                trees within the clearance of a footprint polygon are counted (info: the axis probes do not see corners)
Informational (not gated):
  surface_dz    CIGI HOT (traces against the rendered tiles) at dumped trees, by distance band from the camera, before
                and after the far trip: the tree base height (SampleHeightMostDetailed) minus the rendered surface
  truck         a DIS truck parked among the trees (send_dis_test.py truck-parked --location): COCO visibility and box
  shots         low oblique over the woodland, trees on and off (visual review)

--stages picks a subset (approach,off,gt,truck,oblique,footprints); the gates of stages not run are read back from an existing
report.json in --out, so a single failing stage can be rerun.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
from bench import dense_path as dp  # noqa: E402
from bench import run_bench as rb  # noqa: E402
from bench import scenario  # noqa: E402
from bench.compare import ssim  # noqa: E402

SHOT = "nadir_3km.png"  # the one --smoke shot; named explicitly, never by glob order (each shot is saved twice, with and without _sensor)
EDITOR_LOG = Path.home() / "Library/Logs/CamSimTest/CamSimTest.log"
# Santa Margarita River riparian woodland (MCAS Camp Pendleton). The pose sits on code 10; yaw 45 looks NE over the
# densest woodland (33.31-33.33 N, -117.345..-117.335), which a north-facing camera misses.
RIPARIAN = (33.307, -117.354)
ALT_M = 350.0
YAW = 45.0
PITCH = -20.0
FAR_DLON = 3.41  # the bench's far_origin trip: well past the origin-rebase distance
M_PER_DEG = 111_320.0
WGS84_A = 6_378_137.0
WGS84_E2 = 0.00669437999014


def _deg_per_metre(lat_deg):
    """(deg lat, deg lon) per metre at lat: WGS-84 meridional and prime-vertical radii, as C++ IsClearOf does."""
    lat = np.radians(np.asarray(lat_deg, float))
    w2 = 1.0 - WGS84_E2 * np.sin(lat) ** 2
    m = WGS84_A * (1.0 - WGS84_E2) / (w2 * np.sqrt(w2))
    n = WGS84_A / np.sqrt(w2)
    return np.degrees(1.0 / m), np.degrees(1.0 / (n * np.maximum(np.cos(lat), 1e-6)))
STAGES = ["approach", "off", "gt", "truck", "oblique"]


# ---------------------------------------------------------------- REALISM R4 "deeper" gates (pure)

CELL_M = 100.0
DEFAULT_SHRUB = "shrub_chaparral"  # VegetationMix.cpp DefaultMix
DEFAULT_SHRUB_DENSITY = {20: 6.0, 30: 1.0}
MIN_CANOPY_HEIGHT_M = 3.0


def mix_roles(mix: dict | None, library: dict) -> dict[int, dict[str, dict[int, float]]]:
    """Per land-cover code: {"trees": {library index: weight}, "shrubs": {...}}, each list normalised, as CamSim splits a
    camsim-vegmix-1 mix by the library's roles (VegetationMix.cpp ParseMix); None = the built-in default mix."""
    types = library["types"]
    index = {t["name"]: k for k, t in enumerate(types)}
    role = {k: t.get("role", "tree") for k, t in enumerate(types)}
    out: dict[int, dict[str, dict[int, float]]] = {}
    if mix is None:
        if types and role[0] == "tree":
            out[10] = {"trees": {0: 1.0}, "shrubs": {}}
        if DEFAULT_SHRUB in index and role[index[DEFAULT_SHRUB]] == "shrub":
            for code in DEFAULT_SHRUB_DENSITY:
                out[code] = {"trees": {}, "shrubs": {index[DEFAULT_SHRUB]: 1.0}}
        return out
    for code, names in mix.get("mix", {}).items():
        split: dict[str, dict[int, float]] = {"trees": {}, "shrubs": {}}
        for name, w in names.items():
            if name in index:
                split["shrubs" if role[index[name]] == "shrub" else "trees"][index[name]] = float(w)
        for key in split:
            total = sum(split[key].values())
            split[key] = {k: v / total for k, v in sorted(split[key].items())} if total > 0 else {}
        if split["trees"] or split["shrubs"]:
            out[int(code)] = split
    return out


def _gammaincc(a: float, x: float) -> float:
    """Regularised upper incomplete gamma Q(a, x) (series below a + 1, continued fraction above; Numerical Recipes)."""
    if x <= 0.0:
        return 1.0
    gln = math.lgamma(a)
    if x < a + 1.0:
        ap, total, term = a, 1.0 / a, 1.0 / a
        for _ in range(500):
            ap += 1.0
            term *= x / ap
            total += term
            if abs(term) < abs(total) * 1e-15:
                break
        return max(0.0, 1.0 - total * math.exp(-x + a * math.log(x) - gln))
    b, c, d = x + 1.0 - a, 1e300, 1.0 / (x + 1.0 - a)
    h = d
    for i in range(1, 500):
        an = -i * (i - a)
        b += 2.0
        d = an * d + b
        d = 1e-300 if abs(d) < 1e-300 else d
        c = b + an / c
        c = 1e-300 if abs(c) < 1e-300 else c
        d = 1.0 / d
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < 1e-15:
            break
    return math.exp(-x + a * math.log(x) - gln) * h


def chi2_p(stat: float, dof: int) -> float:
    """P(chi-square with dof degrees of freedom >= stat)."""
    return _gammaincc(dof / 2.0, stat / 2.0)


def _instances(cells: dict, key: str) -> list:
    return [i for c in cells.values() for i in c.get(key, [])]


def placement_gate(cells: dict, roles: dict, code_at, canopy_at=None) -> dict:
    """Trees only on codes whose mix lists trees and, where the canopy layer covers (canopy_at -> (cover, height_m,
    present)), only on texels with cover > 0 and height >= 3 m; shrubs only on codes whose mix lists shrubs."""
    trees, shrubs = _instances(cells, "instances"), _instances(cells, "shrubs")
    tree_codes = {c for c, r in roles.items() if r["trees"]}
    shrub_codes = {c for c, r in roles.items() if r["shrubs"]}
    tc = code_at([i[1] for i in trees], [i[0] for i in trees]) if trees else []
    sc = code_at([i[1] for i in shrubs], [i[0] for i in shrubs]) if shrubs else []
    off_trees = sum(1 for c in tc if int(c) not in tree_codes)
    off_shrubs = sum(1 for c in sc if int(c) not in shrub_codes)
    bare = 0
    if canopy_at is not None and trees:
        for cover, height, present in canopy_at([i[1] for i in trees], [i[0] for i in trees]):
            bare += present and not (cover > 0.0 and height >= MIN_CANOPY_HEIGHT_M)
    return {"pass": bool(trees or shrubs) and off_trees == 0 and off_shrubs == 0 and bare == 0,
            "trees": len(trees), "shrubs": len(shrubs), "trees_off_mix": off_trees, "shrubs_off_mix": off_shrubs,
            "trees_without_canopy": bare}


def species_gate(cells: dict, roles: dict, code_at, min_n: int = 50) -> dict:
    """Per code and role, type counts vs the mix weights: chi-square p > 0.001 (codes with >= min_n instances and two
    or more types); a type the code's mix doesn't list fails outright."""
    out: dict = {"codes": {}}
    ok = True
    for key, role in (("instances", "trees"), ("shrubs", "shrubs")):
        inst = _instances(cells, key)
        if not inst:
            continue
        codes = code_at([i[1] for i in inst], [i[0] for i in inst])
        by_code: dict[int, dict[int, int]] = {}
        for i, c in zip(inst, codes):
            by_code.setdefault(int(c), {}).setdefault(int(i[5]), 0)
            by_code[int(c)][int(i[5])] += 1
        for code, counts in sorted(by_code.items()):
            want = roles.get(code, {}).get(role, {})
            n = sum(counts.values())
            stray = sum(v for t, v in counts.items() if t not in want)
            res = {"n": n, "counts": {str(t): v for t, v in sorted(counts.items())}, "stray": stray}
            if stray:
                res["p"] = 0.0
            elif n >= min_n and len(want) >= 2:
                stat = sum((counts.get(t, 0) - w * n) ** 2 / (w * n) for t, w in want.items())
                res["p"] = chi2_p(stat, len(want) - 1)
            res["pass"] = res.get("p", 1.0) > 0.001
            ok &= res["pass"]
            out["codes"].setdefault(str(code), {})[role] = res
    out["pass"] = ok and bool(out["codes"])
    return out


def cell_of(lat: float, lon: float, cell_m: float = CELL_M) -> tuple[int, int]:
    """VegetationGrid.cpp CellOf."""
    dlat = cell_m / M_PER_DEG
    row = math.floor((lat + 90.0) / dlat)
    dlon = cell_m / (M_PER_DEG * max(math.cos(math.radians(-90.0 + (row + 0.5) * dlat)), 0.01))
    cols = math.ceil(360.0 / dlon)
    return row, min(math.floor(((lon + 180.0) % 360.0) / dlon), cols - 1)


def cell_bounds(row: int, col: int, cell_m: float = CELL_M) -> tuple[float, float, float, float]:
    """(south, north, west, east) degrees, VegetationGrid.cpp CellBounds."""
    dlat = cell_m / M_PER_DEG
    dlon = cell_m / (M_PER_DEG * max(math.cos(math.radians(-90.0 + (row + 0.5) * dlat)), 0.01))
    s, w = -90.0 + row * dlat, -180.0 + col * dlon
    return s, s + dlat, w, min(w + dlon, 180.0)


def cell_area_m2(row: int, col: int, cell_m: float = CELL_M) -> float:
    s, n, w, e = cell_bounds(row, col, cell_m)
    return (n - s) * M_PER_DEG * (e - w) * M_PER_DEG * math.cos(math.radians(0.5 * (s + n)))


def canopy_gate(cells: dict, cover_of, crowns: dict[int, float], lo: float = 0.1, hi: float = 0.5,
                min_cells: int = 50, tol: float = 0.10) -> dict:
    """Per 100 m cell whose mean canopy cover (cover_of(row, col), 0..1) is in [lo, hi]: crown fraction
    sum(pi (r_type x scale)^2) / cell area vs that cover; median |error| <= tol over >= min_cells cells. Above hi one
    tree per candidate saturates (pi r^2 / 34.6 m^2), so those cells are not compared."""
    errs = []
    for (row, col), c in cells.items():
        cover = cover_of(row, col)
        if not lo <= cover <= hi:
            continue
        crown = sum(math.pi * (crowns.get(int(i[5]), 0.0) * i[4]) ** 2 for i in c["instances"])
        errs.append(crown / cell_area_m2(row, col) - cover)
    errs.sort(key=abs)
    med = abs(errs[len(errs) // 2]) if errs else None
    return {"pass": len(errs) >= min_cells and med is not None and med <= tol, "cells": len(errs),
            "median_abs_err": med, "mean_err": (sum(errs) / len(errs)) if errs else None, "range": [lo, hi]}


def far_determinism_gate(far_a: dict, far_b: dict, near: dict, min_common: int = 20) -> dict:
    """Far blocks dumped by two runs: the same hash on every common block; and every far tree whose cell was dumped near
    (in either run) is one of that cell's near trees (same position and type)."""
    common = set(far_a) & set(far_b)
    mismatched = sorted(k for k in common if far_a[k]["hash"] != far_b[k]["hash"])
    near_pts = {}
    for (row, col), c in near.items():
        if not c.get("trees_dumped", True):
            continue  # only the shrub ring dumped it: its trees are unknown
        near_pts[(row, col)] = {(round(i[0], 9), round(i[1], 9), int(i[5])) for i in c["instances"]}
    checked = missing = 0
    for blocks in (far_a, far_b):
        for b in blocks.values():
            for i in b["instances"]:
                key = cell_of(i[0], i[1])
                if key not in near_pts:
                    continue
                checked += 1
                missing += (round(i[0], 9), round(i[1], 9), int(i[5])) not in near_pts[key]
    return {"pass": len(common) >= min_common and not mismatched and missing == 0, "common_blocks": len(common),
            "mismatched": len(mismatched), "far_trees_checked": checked, "not_in_near": missing}


def far_view_gate(far: dict, cam: tuple[float, float, float], yaw_deg: float, half_fov_deg: float, far_m: float,
                  buckets: int = 4, min_trees: int = 200) -> dict:
    """Far trees the camera (lat, lon, HAE m) draws: within half_fov_deg of yaw_deg in bearing and closer in 3D than
    their dissolve bucket's cull (0.7..1.0 x far_m, as BucketCullM). in_view_culled counts the ones in the bearing wedge
    past their cull. Passes with >= min_trees drawn: the far tier really reaches into the view."""
    lat0, lon0, alt = cam
    drawn = culled = 0
    for b in far.values():
        for i in b["instances"]:
            dn = (i[0] - lat0) * M_PER_DEG
            de = (i[1] - lon0) * M_PER_DEG * math.cos(math.radians(lat0))
            off = (math.degrees(math.atan2(de, dn)) - yaw_deg + 180.0) % 360.0 - 180.0
            if abs(off) > half_fov_deg:
                continue
            cull = far_m * (0.7 + 0.3 * int(i[6]) / (buckets - 1)) if buckets > 1 else far_m
            if math.sqrt(dn * dn + de * de + (alt - i[2]) ** 2) < cull:
                drawn += 1
            else:
                culled += 1
    return {"pass": drawn >= min_trees, "in_view": drawn, "in_view_culled": culled}


def handoff_gate(events: list[dict]) -> dict:
    """No gap when a block switches tier: a destroy of one tier's actor while the block's current tier is the other one
    must come after that tier settled (a spawn or an empty) for the block since its latest tier change. A block whose
    tier is none (left both) may drop at once."""
    tier: dict = {}
    settled: dict = {}
    gaps, destroys = 0, 0
    for e in sorted(events, key=lambda x: x["tick"]):
        b = (e["block_row"], e["block_col"])
        if e["ev"] == "tier":
            tier[b] = e["tier"]
            settled[b] = set()
        elif e["ev"] in ("spawn", "empty"):
            settled.setdefault(b, set()).add(e["tier"])
        elif e["ev"] == "destroy":
            destroys += 1
            now = tier.get(b, "none")
            if now not in ("none", e["tier"]) and now not in settled.get(b, set()):
                gaps += 1
    return {"pass": gaps == 0 and bool(events), "gaps": gaps, "destroys": destroys, "events": len(events)}


def r4_identity_gate(cells: dict, r4_cells: dict, min_common: int = 20) -> dict:
    """Every cell both runs dumped has R4's tree hash ("hash" covers the trees only)."""
    common = set(cells) & set(r4_cells)
    mismatched = sorted(k for k in common if cells[k]["hash"] != r4_cells[k]["hash"])
    return {"pass": len(common) >= min_common and not mismatched, "common_cells": len(common),
            "mismatched": len(mismatched), "mismatched_cells": [list(k) for k in mismatched[:10]]}



def load(path: Path) -> np.ndarray:
    """A shot as an RGB array for compare.ssim (which takes the luma and averages 8x8 blocks itself)."""
    return np.asarray(Image.open(path).convert("RGB"))


def pose(lat: float, lon: float, alt: float = ALT_M, yaw: float = YAW, pitch: float = PITCH, fov: float = 60.0
         ) -> scenario.Pose:
    return scenario.Pose(lat, lon, alt, yaw=yaw, gimbal_pitch=pitch, fov_h=fov)


def launch(env_extra: dict, exec_cmds: list[str], host: rb.Host) -> None:
    env = dict(os.environ, CAMSIM_MULTICAST_ADDR="127.0.0.1", CAMSIM_SNAPSHOT_ENDPOINT_ENABLED="1", **env_extra)
    pid_file = REPO / ".cache" / "camsim.pid"
    if rb.camsim_alive(pid_file):
        sys.exit("A CamSim instance is already running (scripts/stop.sh to stop it)")
    rb.wait_port_free(int(rb.HEALTH.rsplit(":", 1)[1]))
    host.thread.start()
    extra = [f"-ExecCmds={','.join(exec_cmds)}"] if exec_cmds else []
    host.launched = True  # from here on the CamSim is this script's: stop() may stop it even if run.sh failed midway
    subprocess.run([str(REPO / "scripts/run.sh"), "--headless", "--local", "--detach", *extra], env=env, check=True,
                   stdout=subprocess.DEVNULL)
    rb.wait_ready(pid_file)
    rb.wait_terrain(120)


def stop(host: rb.Host, log_copy: Path | None = None) -> str:
    """Stop the CamSim this script launched with `host`. If launch() exited before starting one (another instance
    running, port busy), leave whatever is running alone and return no log."""
    host.stop.set()
    if not getattr(host, "launched", False):
        return ""
    subprocess.run([str(REPO / "scripts/stop.sh")], check=False, stdout=subprocess.DEVNULL)
    log = EDITOR_LOG.read_text(errors="replace") if EDITOR_LOG.exists() else ""
    if log_copy:
        log_copy.write_text(log)
    return log


def snapshot(dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    time.sleep(rb.SHOT_SETTLE_S)
    if not rb.fetch_snapshot(dest):
        raise RuntimeError(f"snapshot {dest} failed")


def fly(host: rb.Host, start: tuple[float, float], end: tuple[float, float], seconds: float) -> None:
    t0 = time.monotonic()
    while (t := time.monotonic() - t0) < seconds:
        f = t / seconds
        host.pose = pose(start[0] + f * (end[0] - start[0]), start[1] + f * (end[1] - start[1]))
        time.sleep(0.1)
    host.pose = pose(*end)


def dump_lines(path: Path) -> list[dict]:
    """Every complete line of a dump (CamSim may be mid-write on the last one)."""
    out = []
    for x in path.read_text().splitlines() if path.exists() else []:
        try:
            out.append(json.loads(x))
        except json.JSONDecodeError:
            pass
    return out


def read_dump(path: Path) -> dict[tuple[int, int], dict]:
    """Last spawn of every cell, its tree line (near tier) and its shrub-ring line ("layer": "shrubs") merged. A cell only
    the shrub ring dumped has hash None, no instances and trees_dumped False."""
    cells: dict[tuple[int, int], dict] = {}
    for d in dump_lines(path):
        c = cells.setdefault((d["row"], d["col"]), {"row": d["row"], "col": d["col"], "hash": None, "shrub_hash": None,
                                                    "in_buildings": 0, "instances": [], "shrubs": [], "trees_dumped": False})
        if d.get("layer") == "shrubs":
            c["shrubs"], c["shrub_hash"] = d["shrubs"], d["shrub_hash"]
        else:
            c.update({k: v for k, v in d.items() if k not in ("shrubs", "shrub_hash") or v})
            c["trees_dumped"] = True
    return cells


def respawn_changes(lines: list[dict]) -> tuple[int, int]:
    """(respawned, changed): lines whose (layer, cell) was dumped before, and those whose hash differs from the first."""
    first: dict = {}
    respawned = changed = 0
    for d in lines:
        layer = d.get("layer", "trees")
        key = (layer, d["row"], d["col"])
        h = d["shrub_hash"] if layer == "shrubs" else d["hash"]
        if key in first:
            respawned += 1
            changed += first[key] != h
        else:
            first[key] = h
    return respawned, changed


def wait_settled(dump: Path, quiet_s: float = 6.0, timeout_s: float = 120.0) -> float:
    """Wait until the dump has stopped growing for quiet_s (every wanted cell spawned); returns the seconds waited."""
    t0 = time.monotonic()
    last_n, last_change = -1, t0
    while time.monotonic() - t0 < timeout_s:
        n = len(dump.read_text().splitlines()) if dump.exists() else 0
        if n != last_n:
            last_n, last_change = n, time.monotonic()
        elif n > 0 and time.monotonic() - last_change >= quiet_s:
            break
        time.sleep(1.0)
    rb.wait_terrain(60)
    return time.monotonic() - t0


def dist_m(a: tuple[float, float], b: tuple[float, float]) -> float:
    dn = (b[0] - a[0]) * M_PER_DEG
    de = (b[1] - a[1]) * M_PER_DEG * math.cos(math.radians(a[0]))
    return math.hypot(dn, de)


BBOX_PAD_M = 20.0  # the trees' bounding box is padded by this when looking for footprints
MASK_HALF_DIAG_M = 0.76  # half the diagonal of a mask cell (1.16 m x 0.97 m at 33 N): the raster's own position error
FOOTPRINT_POSES = [  # docs/vegetation.md "Trees vs building footprints": the dense_pass midpoint, then Mainside
    (33.2200, -117.3103, 357.7, 105.0, -30.0),
    (33.3050, -117.3140, 350.0, 0.0, -30.0),
]


def footprint_gate(trees: list[tuple[float, float]], mask, clearance_m: float, polygons: list | None = None) -> dict:
    """REALISM R3b. trees: (lat, lon). mask: .masked(lon, lat) (bench.dense_path.BuildingMask). The runtime rule exactly:
    a tree whose trunk or any of the 4 points clearance_m east, west, north or south of it is masked is a probe hit. With
    the footprint polygons (lon/lat): trunks inside a polygon shrunk by MASK_HALF_DIAG_M (gated) and trees within
    clearance_m of a polygon (info: axis probes do not see a corner diagonally within the clearance)."""
    lat = np.array([t[0] for t in trees], float)
    lon = np.array([t[1] for t in trees], float)
    out: dict = {"trees": len(trees)}
    if not trees:
        out.update(probe_hits=0, trunks_inside_footprints=0, within_clearance=0, mask_cells_in_bbox=0)
        out["pass"] = False  # no trees dumped: the gate has nothing to measure
        out["reasons"] = ["no trees dumped"]
        return out
    reasons: list[str] = []
    k_lat, k_lon = _deg_per_metre(lat)
    dlat = clearance_m * k_lat
    dlon = clearance_m * k_lon
    hit = mask.masked(lon, lat)
    for dx, dy in ((dlon, 0.0), (-dlon, 0.0), (0.0, dlat), (0.0, -dlat)):
        hit |= mask.masked(lon + dx, lat + dy)
    out["probe_hits"] = int(hit.sum())
    if out["probe_hits"]:
        reasons.append(f"{out['probe_hits']} trees with a masked trunk or clearance probe")
    # Fail closed: a mask that is absent or does not reach the trees reads "no buildings" everywhere, so the probes alone
    # would pass. Require mask cells set within the trees' bounding box (padded by BBOX_PAD_M).
    pad_lat = BBOX_PAD_M / M_PER_DEG
    pad_lon = BBOX_PAD_M / (M_PER_DEG * math.cos(math.radians(float(lat.mean()))))
    w, e = float(lon.min()) - pad_lon, float(lon.max()) + pad_lon
    south, n = float(lat.min()) - pad_lat, float(lat.max()) + pad_lat
    nx = min(2000, max(2, int((e - w) * M_PER_DEG * math.cos(math.radians(float(lat.mean()))))))
    ny = min(2000, max(2, int((n - south) * M_PER_DEG)))
    gx, gy = np.meshgrid(np.linspace(w, e, nx), np.linspace(south, n, ny))
    out["mask_cells_in_bbox"] = int(np.count_nonzero(mask.masked(gx, gy)))
    if out["mask_cells_in_bbox"] == 0:
        reasons.append("the mask has no footprint within the dumped trees' bounding box (wrong or empty mask)")
    if not polygons:
        reasons.append("no footprint polygons (buildings layer or Overture snapshot unavailable): "
                       "trunks inside footprints not checked")
    else:
        import shapely
        from shapely import affinity

        k = M_PER_DEG * math.cos(math.radians(float(lat.mean())))
        polys = [affinity.affine_transform(p, [k, 0.0, 0.0, M_PER_DEG, 0.0, 0.0]) for p in polygons]
        pts = shapely.points(lon * k, lat * M_PER_DEG)
        box = shapely.box(w * k, south * M_PER_DEG, e * k, n * M_PER_DEG)
        out["footprints_in_bbox"] = len(shapely.STRtree(polys).query(box, predicate="intersects"))
        if out["footprints_in_bbox"] == 0:
            reasons.append("no footprint polygon within the dumped trees' bounding box")
        deep = [g for g in (p.buffer(-MASK_HALF_DIAG_M) for p in polys) if not g.is_empty]
        inside = shapely.STRtree(deep).query(pts, predicate="within")[0] if deep else np.zeros(0, int)
        near = shapely.STRtree(polys).query(pts, predicate="dwithin", distance=clearance_m)[0]
        out["trunks_inside_footprints"] = len(np.unique(inside))
        out["within_clearance"] = len(np.unique(near))
        if out["trunks_inside_footprints"]:
            reasons.append(f"{out['trunks_inside_footprints']} trunks inside a footprint")
    out["pass"] = not reasons
    if reasons:
        out["reasons"] = reasons
    return out


def load_footprints(scene: Path) -> list | None:
    """The package's kept footprint polygons (lon/lat) from its cached Overture snapshot (same filter as the buildings
    layer, minus the terrain-height drop, which keeps all at Pendleton); None when the snapshot is not in the cache."""
    from camsim_scene import buildings as B
    from camsim_scene.cache import Cache, default_cache_root
    from camsim_scene.manifest import Manifest

    m = Manifest.load(scene / "manifest.json")
    if not m.layers.get("buildings"):
        return None
    a = m.source(m.layers["buildings"]["priorities"][0]).assets[0]
    hit = Cache(default_cache_root()).lookup(a.url, a.sha256)
    if hit is None:
        return None
    area = float(m.layers["buildings"]["min_area_m2"])
    return [p for fp in B.read_snapshot(hit[0]) for p in B.clean(fp.geometry, area)]


def footprints_gate(a: argparse.Namespace, report: dict) -> None:
    run = a.out / "footprints"
    dump = run / "cells.jsonl"
    run.mkdir(parents=True, exist_ok=True)
    dump.unlink(missing_ok=True)
    host = rb.Host(scenario.SITES["pendleton"])
    host.pose = pose(*FOOTPRINT_POSES[0])
    try:
        launch({"CAMSIM_SCENE_DIR": str(a.scene)}, [f"camsim.Vegetation.DumpPath {dump}"], host)
        for k, p in enumerate(FOOTPRINT_POSES):
            host.pose = pose(*p)
            time.sleep(5.0)
            wait_settled(dump)
            snapshot(run / f"pose{k}.png")
    finally:
        log = stop(host, run / "CamSimTest.log")
    cells = read_dump(dump)
    trees = [(i[0], i[1]) for d in cells.values() for i in d["instances"]]
    mask_dir = a.scene / "features" / "buildings" / "mask"
    clearance = float(os.environ.get("CAMSIM_VEGETATION_BUILDING_CLEARANCE_M", "2.0"))
    res = footprint_gate(trees, dp.BuildingMask(mask_dir), clearance, load_footprints(a.scene))
    if not (mask_dir / "index.json").is_file() or not any(mask_dir.glob("*.png")):  # fail closed: no mask to test against
        res["pass"] = False
        res.setdefault("reasons", []).append(f"{mask_dir} has no index.json or no tile")
    line = next((x for x in log.splitlines() if "Vegetation: on (" in x), "")
    res["log_line"] = line.split("LogCamSim: ", 1)[-1]
    res["cells"] = len(cells)
    res["pass"] = res["pass"] and "buildings: mask" in line
    report["footprints"] = res


def surface_dz(host: rb.Host, cells: dict, centre: tuple[float, float], per_band: int = 25) -> dict:
    """Tree base (most-detailed height) minus CIGI HOT at that point (a trace against the rendered tiles), by distance
    band from the camera. Positive = the tree floats above the drawn ground."""
    import ocean_check as oc  # HAT/HOT request packing and the response listener

    # Up to radius_m (1000 m default): the dump also holds cells dropped since (approach start), which are not drawn.
    bands = {"0-300m": (0, 300), "300-700m": (300, 700), "700-1000m": (700, 1000)}
    trees = [(dist_m(centre, (i[0], i[1])), i) for d in cells.values() for i in d["instances"]]
    rng = np.random.default_rng(1)
    responses = oc.Responses()
    out: dict = {}
    rid = 1000
    try:
        for name, (lo, hi) in bands.items():
            pick = [t for t in trees if lo <= t[0] < hi]
            if len(pick) > per_band:
                pick = [pick[k] for k in sorted(rng.choice(len(pick), per_band, replace=False))]
            dz = []
            for _, i in pick:
                rid += 1
                oc.send_packets(host, oc.pack_hat_hot_request(rid, i[0], i[1]))
                deadline = time.time() + 2.0
                while rid not in responses.hot and time.time() < deadline:
                    time.sleep(0.02)
                r = responses.hot.get(rid)
                if r and r.get("valid") and "hot" in r:
                    dz.append(i[2] - r["hot"])
            out[name] = {
                "n": len(dz),
                "asked": len(pick),
                "max_abs_dz_m": round(float(np.max(np.abs(dz))), 3) if dz else None,
                "p50_abs_dz_m": round(float(np.median(np.abs(dz))), 3) if dz else None,
                "mean_dz_m": round(float(np.mean(dz)), 3) if dz else None,
                "max_dist_m": round(max((t[0] for t in pick), default=0.0)),
            }
    finally:
        responses.stop.set()
    return out


def run_approach(label: str, scene: Path, out: Path, d_lat: float, env_extra: dict | None = None) -> dict:
    run = out / label
    dump, far_dump, events = run / "cells.jsonl", run / "far.jsonl", run / "events.jsonl"
    run.mkdir(parents=True, exist_ok=True)
    for f in (dump, far_dump, events):
        f.unlink(missing_ok=True)
    host = rb.Host(scenario.SITES["pendleton"])
    host.pose = pose(RIPARIAN[0] + d_lat, RIPARIAN[1])
    res: dict = {"dump": str(dump), "far_dump": str(far_dump), "events": str(events)}
    try:
        launch({"CAMSIM_SCENE_DIR": str(scene), **(env_extra or {})},
               [f"camsim.Vegetation.DumpPath {dump}", f"camsim.Vegetation.FarDumpPath {far_dump}",
                f"camsim.Vegetation.EventsPath {events}", "camsim.Vegetation.GroundCheck 1"], host)
        # The ground check measures a settled view: hold the start pose until it has logged.
        deadline = time.monotonic() + 120.0
        while time.monotonic() < deadline and "Vegetation: ground check" not in EDITOR_LOG.read_text(errors="replace"):
            time.sleep(1.0)
        fly(host, (RIPARIAN[0] + d_lat, RIPARIAN[1]), RIPARIAN, 30.0)
        res["settle_s"] = round(wait_settled(dump), 1)
        res["surface_dz_before"] = surface_dz(host, read_dump(dump), RIPARIAN)
        snapshot(run / "a.png")
        n_before = len(dump_lines(dump))
        far = (RIPARIAN[0], RIPARIAN[1] + FAR_DLON)
        host.pose = pose(*far)
        time.sleep(20.0)
        host.pose = pose(*RIPARIAN)
        time.sleep(5.0)
        res["resettle_s"] = round(wait_settled(dump), 1)
        res["surface_dz_after"] = surface_dz(host, read_dump(dump), RIPARIAN)
        snapshot(run / "b.png")
        res["n_lines_before_trip"] = n_before
    finally:
        log = stop(host, run / "CamSimTest.log")
    ground = [x for x in log.splitlines() if "Vegetation: ground check" in x]
    res["ground"] = ground[-1] if ground else None
    res["veg_log"] = [x.split("LogCamSim: ", 1)[-1] for x in log.splitlines()
                      if "Vegetation" in x and ("Warning" in x or "Error" in x or "Vegetation: o" in x)][:20]
    return res


def parse_ground(line: str | None) -> dict:
    if not line:
        return {"pass": False, "why": "no ground check line"}
    kv = dict(p.split("=") for p in line.split("ground check ")[1].split())
    return {"pass": int(kv["n"]) == 200 and float(kv["max_dz_m"]) < 0.5, **kv}


def approach_gates(a: argparse.Namespace, report: dict) -> None:
    north = run_approach("north", a.scene, a.out, +3000.0 / M_PER_DEG)
    south = run_approach("south", a.scene, a.out, -3000.0 / M_PER_DEG)
    dn, ds = read_dump(Path(north["dump"])), read_dump(Path(south["dump"]))

    lc = dp.LandCover(a.scene / "landcover")

    def code_at(lon, lat):
        return lc.codes(np.array(lon, float), np.array(lat, float)).tolist()

    roles = mix_roles(read_mix(a.scene), read_library())
    canopy = Canopy(a.scene) if (a.scene / "canopy" / "cover" / "index.json").exists() else None
    both = {**dn, **ds}
    report["placement"] = placement_gate(both, roles, code_at, canopy.at if canopy else None) | {"cells": len(both)}
    report["species"] = species_gate(both, roles, code_at)
    if canopy:
        lib = read_library()["types"]
        crowns = {k: float(t.get("crown_radius_m", 0.3 * t["height_m"])) for k, t in enumerate(lib)}
        report["canopy"] = canopy_gate(both, canopy.cell_cover, crowns)
    far_n, far_s = read_far_dump(Path(north["far_dump"])), read_far_dump(Path(south["far_dump"]))
    if far_n or far_s:
        report["far_determinism"] = far_determinism_gate(far_n, far_s, both)
    events = [json.loads(x) for r in (north, south) for x in Path(r["events"]).read_text().splitlines()
              if Path(r["events"]).exists()]
    if events:
        report["handoff"] = handoff_gate(events)

    report["grounding"] = {"north": parse_ground(north["ground"]), "south": parse_ground(south["ground"])}
    report["grounding"]["pass"] = report["grounding"]["north"]["pass"] and report["grounding"]["south"]["pass"]

    common = set(dn) & set(ds)
    mismatched = sorted(k for k in common if dn[k]["hash"] != ds[k]["hash"])
    report["determinism"] = {"pass": len(common) > 20 and not mismatched, "common_cells": len(common),
                             "only_north": len(set(dn) - set(ds)), "only_south": len(set(ds) - set(dn)),
                             "mismatched": len(mismatched), "mismatched_cells": mismatched[:10]}

    floor = ssim(load(a.baseline / "a" / "shots" / SHOT), load(a.baseline / "b" / "shots" / SHOT))
    shift = {}
    for label, res in (("north", north), ("south", south)):
        respawned, changed = respawn_changes(dump_lines(Path(res["dump"])))
        s = ssim(load(a.out / label / "a.png"), load(a.out / label / "b.png"))
        shift[label] = {"ssim": s, "respawned_cells": respawned, "hash_changed": changed,
                        "pass": s >= floor - 0.01 and respawned > 0 and changed == 0}
    report["origin_shift"] = {"pass": shift["north"]["pass"] and shift["south"]["pass"], "baseline_floor": floor, **shift}

    # Informational: tree base vs the drawn surface, and the terrain tileset still found after the trip.
    report["surface_dz"] = {label: {"before_trip": r["surface_dz_before"], "after_trip": r["surface_dz_after"],
                                    "settle_s": r["settle_s"], "resettle_s": r["resettle_s"]}
                            for label, r in (("north", north), ("south", south))}
    report["veg_log"] = {"north": north["veg_log"], "south": south["veg_log"]}


LIBRARY = REPO / "unreal_project" / "CamSimTest" / "Content" / "NonUFS" / "Vegetation" / "library.json"
BUDGET_GAME_P95_MS = 3.0


def read_library() -> dict:
    return json.loads(LIBRARY.read_text())


def read_mix(scene: Path) -> dict | None:
    p = scene / "vegetation" / "mix.json"
    return json.loads(p.read_text()) if p.exists() else None


def read_far_dump(path: Path) -> dict:
    if not path.exists():
        return {}
    out = {}
    for x in path.read_text().splitlines():
        d = json.loads(x)
        out.setdefault((d["block_row"], d["block_col"]), d)
    return out


CANOPY_NO_DATA = 255


class Canopy:
    """The package's canopy tile sets: (cover 0..1, height m, present) at points; mean cover of a 100 m cell."""

    def __init__(self, scene: Path) -> None:
        self.cover = dp.LandCover(scene / "canopy" / "cover")
        self.height = dp.LandCover(scene / "canopy" / "height")

    def at(self, lon, lat) -> list:
        lon, lat = np.array(lon, float), np.array(lat, float)
        ti = np.floor(np.round(lat / dp.TILE_DEG, 9)).astype(np.int64)
        tj = np.floor(np.round(lon / dp.TILE_DEG, 9)).astype(np.int64)
        present = [self.cover._tile(int(i), int(j)) is not None for i, j in zip(ti, tj)]
        c, h = self.cover.codes(lon, lat), self.height.codes(lon, lat)
        # cover 255 = no CHM pixel there: CamSim uses the land-cover rule, as outside the layer
        return [(min(int(cv), 100) / 100.0, float(hv), p and int(cv) != CANOPY_NO_DATA) for cv, hv, p in zip(c, h, present)]

    def cell_cover(self, row: int, col: int) -> float:
        s, n, w, e = cell_bounds(row, col)
        g = (np.arange(10) + 0.5) / 10.0
        lat, lon = np.meshgrid(s + g * (n - s), w + g * (e - w))
        c = self.cover.codes(lon.ravel(), lat.ravel())
        c = c[c != CANOPY_NO_DATA]
        return float(np.minimum(c, 100).mean() / 100.0) if c.size else -1.0   # -1: no data, never in a gate's range


def long_range_gate(a: argparse.Namespace, report: dict) -> None:
    """The bench's long_range phase (650 m HAE oblique over the far tier) with vegetation on and off: 0 dropped frames
    with it on, game p95 within BUDGET_GAME_P95_MS of off, and far trees drawn in the view (long_range_view)."""
    res = {}
    for label, on in (("on", "1"), ("off", "0")):
        out = a.out / "long_range" / label
        subprocess.run([sys.executable, str(REPO / "scripts/bench/run_bench.py"), "--label", f"veg_long_{label}",
                        "--site", "pendleton", "--phases", "long_range", "--out", str(out)],
                       env={**os.environ, "CAMSIM_SCENE_DIR": str(a.scene), "CAMSIM_VEGETATION_ENABLED": on}, check=True)
        res[label] = json.loads((out / "results.json").read_text())["phases"]["long_range"]
    d95 = res["on"]["game_ms_p95"] - res["off"]["game_ms_p95"]
    view = long_range_view(a)
    report["long_range"] = {"pass": res["on"]["dropped"] == 0 and d95 <= BUDGET_GAME_P95_MS and view["pass"],
                            "game_p95_delta_ms": d95, "dropped_on": res["on"]["dropped"], "far_view": view,
                            **{f"{k}_{lbl}": res[lbl][k] for lbl in res for k in ("game_ms_p50", "game_ms_p95", "gpu_ms_p50")}}


LONG_RANGE_FAR_M = 4000.0  # vegetation.far_radius_m default


def long_range_view(a: argparse.Namespace) -> dict:
    """Hold the long_range phase's mid pose with the far dump on until it settles: far trees drawn in the view
    (far_view_gate), and a shot for the visual review."""
    run = a.out / "long_range" / "view"
    run.mkdir(parents=True, exist_ok=True)
    far_dump = run / "far.jsonl"
    far_dump.unlink(missing_ok=True)
    phase = next(p for p in scenario.build_phases(site=scenario.SITES["pendleton"]) if p.name == "long_range")
    p = phase.pose_at(phase.duration_s / 2)
    host = rb.Host(scenario.SITES["pendleton"])
    host.pose = p
    try:
        launch({"CAMSIM_SCENE_DIR": str(a.scene)}, [f"camsim.Vegetation.FarDumpPath {far_dump}"], host)
        wait_settled(far_dump, quiet_s=10.0, timeout_s=180.0)
        snapshot(run / "long_range_view.png")
    finally:
        stop(host, run / "CamSimTest.log")
    return far_view_gate(read_far_dump(far_dump), (p.lat, p.lon, p.alt), yaw_deg=p.yaw + p.gimbal_yaw,
                         half_fov_deg=p.fov_h / 2, far_m=LONG_RANGE_FAR_M) | {"pose": [p.lat, p.lon, p.alt]}


def r4_identity_stage(a: argparse.Namespace, report: dict) -> None:
    """A package without canopy or mix (R4's), shrubs and the far tier off: every cell has the R4 run's tree hash."""
    run = run_approach("r4_identity", a.r4_scene, a.out, +3000.0 / M_PER_DEG,
                       {"CAMSIM_VEGETATION_SHRUBS_ENABLED": "0", "CAMSIM_VEGETATION_FAR_RADIUS_M": "0"})
    report["r4_identity"] = r4_identity_gate(read_dump(Path(run["dump"])), read_dump(a.r4_baseline / "north" / "cells.jsonl"))


def smoke_run(label: str, out: Path, scene: Path, enabled: str) -> str:
    """One run_bench.py --smoke at Pendleton with vegetation on or off; returns (and saves) that run's editor log."""
    subprocess.run([sys.executable, str(REPO / "scripts/bench/run_bench.py"), "--label", label, "--site", "pendleton",
                    "--smoke", "--out", str(out)], env={**os.environ, "CAMSIM_SCENE_DIR": str(scene),
                                                        "CAMSIM_VEGETATION_ENABLED": enabled}, check=True)
    log = EDITOR_LOG.read_text(errors="replace") if EDITOR_LOG.exists() else ""
    (out / "CamSimTest.log").write_text(log)
    return log


def off_gate(a: argparse.Namespace, report: dict) -> None:
    """Off run vs the pre-slice baseline. Weak by construction: the smoke has one shot, nadir from 3 km, where trees are
    a few pixels, so a trees-on run is measured beside it to show the margin the gate has."""
    off, on = a.out / "off", a.out / "on_smoke"
    log_off = smoke_run("veg-off", off, a.scene, "0")
    # vegetation.enabled: false creates no manager and logs nothing about vegetation; on, it logs "Vegetation: on (...)"
    # (or "Vegetation: off (why)"). The log must be this run's: it carries the startup diagnostics line.
    if "CamSim startup diagnostics" not in log_off:
        disabled = {"checked": False, "why": f"no CamSim startup line in {EDITOR_LOG}"}
    else:
        disabled = {"checked": True, "disabled": "Vegetation: on (" not in log_off and "Vegetation: off (" not in log_off}
    smoke_run("veg-on", on, a.scene, "1")
    rows = []
    for shot in sorted(p for p in (a.baseline / "a" / "shots").glob("*.png") if not p.stem.endswith("_sensor")):
        b_floor = ssim(load(shot), load(a.baseline / "b" / "shots" / shot.name))
        s_off_a = ssim(load(shot), load(off / "shots" / shot.name))
        s_off_b = ssim(load(a.baseline / "b" / "shots" / shot.name), load(off / "shots" / shot.name))
        s_on_a = ssim(load(shot), load(on / "shots" / shot.name))
        s_on_b = ssim(load(a.baseline / "b" / "shots" / shot.name), load(on / "shots" / shot.name))
        rows.append({"shot": shot.name, "ssim_off_vs_a": s_off_a, "ssim_off_vs_b": s_off_b, "baseline": b_floor,
                     "ssim_trees_on_vs_a": s_on_a, "ssim_trees_on_vs_b": s_on_b,
                     "trees_on_would_pass": s_on_a >= b_floor - 0.005, "pass": s_off_a >= b_floor - 0.005})
    report["off_switch"] = {"pass": bool(rows) and all(r["pass"] for r in rows) and disabled.get("disabled", True),
                            "vegetation_disabled_in_log": disabled, "shots": rows}


def gt_gate(a: argparse.Namespace, report: dict) -> None:
    # Its own CamSim runs, on its default (San Francisco) config: trees from the committed SF land-cover sample.
    env = {k: v for k, v in os.environ.items() if k != "CAMSIM_SCENE_DIR"}
    gt = subprocess.run(["uv", "run", "-q", "--with", "numpy", "--with", "pillow", "--with", "pycocotools", "python",
                         str(REPO / "scripts/gt_occlusion_check.py"), str(a.out / "gt")], env=env, check=False)
    report["gt"] = {"pass": gt.returncode == 0, "exit": gt.returncode}


def densest_tree(cells: dict, near: tuple[float, float], within_m: float = 400.0) -> list[float]:
    """The dumped tree with the most neighbours within 8 m, among trees within_m of `near`."""
    trees = [i for d in cells.values() for i in d["instances"] if dist_m(near, (i[0], i[1])) < within_m]
    best, best_n = trees[0], -1
    for t in trees:
        n = sum(1 for u in trees if dist_m((t[0], t[1]), (u[0], u[1])) < 8.0)
        if n > best_n:
            best, best_n = t, n
    return best


def truck_info(a: argparse.Namespace, report: dict) -> None:
    import send_dis_test as sd

    cells = read_dump(a.out / "north" / "cells.jsonl")
    if not cells:
        report["truck"] = {"why": "no north dump (run the approach stage first)"}
        return
    tree = densest_tree(cells, RIPARIAN)
    preset = sd.PRESETS["truck-parked"]
    n0, e0, _, _ = sd.PathFollower(preset.waypoints_ne, preset.speed_mps).state(0.0)
    # Truck 2 m east of the tree's trunk: re-centre the preset so its loop start lands there.
    m_lat, m_lon = M_PER_DEG, M_PER_DEG * math.cos(math.radians(tree[0]))
    truck = (tree[0], tree[1] + 2.0 / m_lon)
    centre = (truck[0] - n0 / m_lat, truck[1] - e0 / m_lon)
    run = a.out / "truck"
    run.mkdir(parents=True, exist_ok=True)
    host = rb.Host(scenario.SITES["pendleton"])
    # 45 deg down from 85 m south and 85 m up, 15 deg FOV (~31 m across at the truck).
    cam = pose(truck[0] - 85.0 / m_lat, truck[1], tree[2] + 85.0, yaw=0.0, pitch=-45.0, fov=15.0)
    host.pose = cam
    sender = None
    try:
        launch({"CAMSIM_SCENE_DIR": str(a.scene), "CAMSIM_DIS_ENABLED": "1", "CAMSIM_ML_ENABLED": "1",
                "CAMSIM_ML_OUTPUT_DIR": str(run / "ml"), "CAMSIM_ML_DEPTH_ENABLED": "0",
                "CAMSIM_ML_INTERVAL_FRAMES": "3"}, [], host)
        sender = subprocess.Popen([sys.executable, str(REPO / "scripts/send_dis_test.py"), "truck-parked",
                                   "--location", f"{centre[0]:.8f},{centre[1]:.8f}"], stdout=subprocess.DEVNULL)
        time.sleep(25.0)
        rb.wait_terrain(60)
        snapshot(run / "truck_trees.png")
        nadir = pose(truck[0], truck[1], tree[2] + 120.0, yaw=0.0, pitch=-90.0, fov=15.0)
        host.pose = nadir
        time.sleep(8.0)
        snapshot(run / "truck_trees_nadir.png")
    finally:
        if sender:
            sender.terminate()
        stop(host, run / "CamSimTest.log")
    anns = []
    for f in sorted((run / "ml").rglob("*.jsonl")):
        for line in f.read_text().splitlines():
            if line.startswith("{"):
                rec = json.loads(line)
                anns += [(rec.get("frame_id", rec.get("image_id")), x) for x in rec.get("annotations", [])
                         if x.get("category", {}).get("name") == "truck"]
    vis = [x.get("visibility") for _, x in anns if x.get("visibility") is not None]
    last = anns[-1][1] if anns else {}
    report["truck"] = {"tree": tree[:3], "truck_lat_lon": truck, "annotations": len(anns),
                       "visibility_min": min(vis) if vis else None,
                       "visibility_median": float(np.median(vis)) if vis else None,
                       "last_bbox": last.get("bbox"), "last_visibility": last.get("visibility"),
                       "shots": [str(run / "truck_trees.png"), str(run / "truck_trees_nadir.png")]}


def oblique_shots(a: argparse.Namespace, report: dict) -> None:
    """Low oblique over the woodland, trees on and off (gate 8, the user's visual review)."""
    shots = {}
    for label, on in (("on", "1"), ("off", "0")):
        host = rb.Host(scenario.SITES["pendleton"])
        low = pose(RIPARIAN[0], RIPARIAN[1], 120.0, yaw=YAW, pitch=-12.0)
        host.pose = low
        try:
            launch({"CAMSIM_SCENE_DIR": str(a.scene), "CAMSIM_VEGETATION_ENABLED": on}, [], host)
            time.sleep(25.0)
            rb.wait_terrain(60)
            snapshot(a.out / "oblique" / f"low_oblique_trees_{label}.png")
            host.pose = pose(*RIPARIAN)
            time.sleep(10.0)
            rb.wait_terrain(60)
            snapshot(a.out / "oblique" / f"riparian_trees_{label}.png")
        finally:
            stop(host, a.out / "oblique" / f"CamSimTest_{label}.log")
        shots[label] = [str(a.out / "oblique" / f"low_oblique_trees_{label}.png"),
                        str(a.out / "oblique" / f"riparian_trees_{label}.png")]
    report["shots"] = {"pose_low": [RIPARIAN[0], RIPARIAN[1], 120.0, YAW, -12.0],
                       "pose_riparian": [RIPARIAN[0], RIPARIAN[1], ALT_M, YAW, PITCH], **shots}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scene", type=Path, required=True)
    ap.add_argument("--baseline", type=Path, required=True, help="Task 1 Step 0 runs (a/, b/ with shots/)")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--stages", default=",".join(STAGES),
                    help=f"comma list of {','.join(STAGES)}, footprints (R3b), long_range, r4_identity (R4; not in the default)")
    ap.add_argument("--r4-scene", type=Path, default=REPO / ".cache" / "scene-packages" / "pendleton-r2",
                    help="r4_identity: a package without canopy or mix")
    ap.add_argument("--r4-baseline", type=Path, default=REPO / ".cache" / "veg" / "check-i1-r2",
                    help="r4_identity: an R4 run of vegetation_check.py on --r4-scene (north/cells.jsonl)")
    a = ap.parse_args()
    a.scene, a.baseline, a.out = a.scene.resolve(), a.baseline.resolve(), a.out.resolve()
    a.r4_scene, a.r4_baseline = a.r4_scene.resolve(), a.r4_baseline.resolve()
    a.out.mkdir(parents=True, exist_ok=True)
    report_path = a.out / "report.json"
    report: dict = json.loads(report_path.read_text()) if report_path.exists() else {}
    report.pop("pass", None)
    stages = [s for s in a.stages.split(",") if s]
    run = {"approach": approach_gates, "off": off_gate, "gt": gt_gate, "truck": truck_info, "oblique": oblique_shots,
           "footprints": footprints_gate, "long_range": long_range_gate, "r4_identity": r4_identity_stage}
    for s in stages:
        print(f"[veg] stage {s}", flush=True)
        run[s](a, report)
        report_path.write_text(json.dumps(report, indent=2))

    gates = ["placement", "grounding", "determinism", "origin_shift", "off_switch", "gt"]
    gates += [g for g in ("footprints", "species", "canopy", "far_determinism", "handoff", "long_range", "r4_identity")
              if g in report]
    report["pass"] = all(report.get(g, {}).get("pass", False) for g in gates)
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps({g: report.get(g, {}).get("pass") for g in gates} | {"pass": report["pass"]}, indent=2))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
