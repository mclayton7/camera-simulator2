#!/usr/bin/env python3
# Copyright CamSim Contributors. All Rights Reserved.
"""Vegetation acceptance (REALISM R4 slice, docs/vegetation.md): placement on code 10, grounding, determinism from two
approach paths, origin-shift stability, off switch vs the pre-slice baseline, ground-truth check still passing.

  caffeinate -ims uv run --project scripts/scene --with pycocotools python scripts/vegetation_check.py \
      --scene "$PKG" --baseline .cache/veg/baseline --out .cache/veg/check

PKG is the scene package the baseline was captured with (Pendleton R2: .cache/scene-packages/pendleton-r2, absolute).
The baseline is the pre-slice `run_bench.py --smoke --site pendleton` pair (a/, b/ with shots/).

Gates (report.json, exit 0 only when all pass):
  placement     every dumped tree's (lat, lon) is land-cover code 10 (read with bench/dense_path.LandCover)
  grounding     camsim.Vegetation.GroundCheck: n == 200 and max_dz_m < 0.5, both runs
  determinism   approach from 3 km north and 3 km south: every cell in both dumps has the same hash
  origin_shift  /snapshot at the riparian pose before and after a trip to lon + 3.41: SSIM >= baseline floor - 0.01,
                and every cell spawned again after the trip has its first hash
  off_switch    CAMSIM_VEGETATION_ENABLED=0 smoke shots vs the baseline: SSIM >= the baseline's own pair SSIM - 0.005
  gt            scripts/gt_occlusion_check.py exits 0 (vegetation on)
Informational (not gated):
  surface_dz    CIGI HOT (traces against the rendered tiles) at dumped trees, by distance band from the camera, before
                and after the far trip: the tree base height (SampleHeightMostDetailed) minus the rendered surface
  truck         a DIS truck parked among the trees (send_dis_test.py truck-parked --location): COCO visibility and box
  shots         low oblique over the woodland, trees on and off (visual review)

--stages picks a subset (approach,off,gt,truck,oblique); the gates of stages not run are read back from an existing
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
STAGES = ["approach", "off", "gt", "truck", "oblique"]


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
    subprocess.run([str(REPO / "scripts/run.sh"), "--headless", "--local", "--detach", *extra], env=env, check=True,
                   stdout=subprocess.DEVNULL)
    rb.wait_ready(pid_file)
    rb.wait_terrain(120)


def stop(host: rb.Host, log_copy: Path | None = None) -> str:
    host.stop.set()
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
    """Last spawn of every cell."""
    return {(d["row"], d["col"]): d for d in dump_lines(path)}


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


def run_approach(label: str, scene: Path, out: Path, d_lat: float) -> dict:
    run = out / label
    dump = run / "cells.jsonl"
    run.mkdir(parents=True, exist_ok=True)
    dump.unlink(missing_ok=True)
    host = rb.Host(scenario.SITES["pendleton"])
    host.pose = pose(RIPARIAN[0] + d_lat, RIPARIAN[1])
    res: dict = {"dump": str(dump)}
    try:
        launch({"CAMSIM_SCENE_DIR": str(scene)},
               [f"camsim.Vegetation.DumpPath {dump}", "camsim.Vegetation.GroundCheck 1"], host)
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
    inst = [i for d in list(dn.values()) + list(ds.values()) for i in d["instances"]]  # each i = [lat, lon, h, yaw, s]
    codes = lc.codes(np.array([i[1] for i in inst], float), np.array([i[0] for i in inst], float)) if inst else np.zeros(0)
    wrong, total = int((codes != 10).sum()), len(inst)
    report["placement"] = {"pass": total > 0 and wrong == 0, "instances": total, "not_on_code_10": wrong,
                           "cells": len(set(dn) | set(ds))}

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
        lines = dump_lines(Path(res["dump"]))
        first: dict = {}
        respawned = changed = 0
        for k, d in enumerate(lines):
            key = (d["row"], d["col"])
            if key in first:
                respawned += 1
                changed += first[key] != d["hash"]
            else:
                first[key] = d["hash"]
        s = ssim(load(a.out / label / "a.png"), load(a.out / label / "b.png"))
        shift[label] = {"ssim": s, "respawned_cells": respawned, "hash_changed": changed,
                        "pass": s >= floor - 0.01 and respawned > 0 and changed == 0}
    report["origin_shift"] = {"pass": shift["north"]["pass"] and shift["south"]["pass"], "baseline_floor": floor, **shift}

    # Informational: tree base vs the drawn surface, and the terrain tileset still found after the trip.
    report["surface_dz"] = {label: {"before_trip": r["surface_dz_before"], "after_trip": r["surface_dz_after"],
                                    "settle_s": r["settle_s"], "resettle_s": r["resettle_s"]}
                            for label, r in (("north", north), ("south", south))}
    report["veg_log"] = {"north": north["veg_log"], "south": south["veg_log"]}


def off_gate(a: argparse.Namespace, report: dict) -> None:
    off = a.out / "off"
    subprocess.run([sys.executable, str(REPO / "scripts/bench/run_bench.py"), "--label", "veg-off", "--site", "pendleton",
                    "--smoke", "--out", str(off)], env={**os.environ, "CAMSIM_SCENE_DIR": str(a.scene),
                                                        "CAMSIM_VEGETATION_ENABLED": "0"}, check=True)
    rows = []
    for shot in sorted(p for p in (a.baseline / "a" / "shots").glob("*.png") if not p.stem.endswith("_sensor")):
        b_floor = ssim(load(shot), load(a.baseline / "b" / "shots" / shot.name))
        s_off_a = ssim(load(shot), load(off / "shots" / shot.name))
        s_off_b = ssim(load(a.baseline / "b" / "shots" / shot.name), load(off / "shots" / shot.name))
        rows.append({"shot": shot.name, "ssim_off_vs_a": s_off_a, "ssim_off_vs_b": s_off_b, "baseline": b_floor,
                     "pass": s_off_a >= b_floor - 0.005})
    report["off_switch"] = {"pass": bool(rows) and all(r["pass"] for r in rows), "shots": rows}


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
    ap.add_argument("--stages", default=",".join(STAGES), help=f"comma list of {','.join(STAGES)}")
    a = ap.parse_args()
    a.scene, a.baseline, a.out = a.scene.resolve(), a.baseline.resolve(), a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=True)
    report_path = a.out / "report.json"
    report: dict = json.loads(report_path.read_text()) if report_path.exists() else {}
    report.pop("pass", None)
    stages = [s for s in a.stages.split(",") if s]
    run = {"approach": approach_gates, "off": off_gate, "gt": gt_gate, "truck": truck_info, "oblique": oblique_shots}
    for s in stages:
        print(f"[veg] stage {s}", flush=True)
        run[s](a, report)
        report_path.write_text(json.dumps(report, indent=2))

    gates = ["placement", "grounding", "determinism", "origin_shift", "off_switch", "gt"]
    report["pass"] = all(report.get(g, {}).get("pass", False) for g in gates)
    report_path.write_text(json.dumps(report, indent=2))
    print(json.dumps({g: report.get(g, {}).get("pass") for g in gates} | {"pass": report["pass"]}, indent=2))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
