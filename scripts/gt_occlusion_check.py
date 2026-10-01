#!/usr/bin/env python3
"""Acceptance check: render-measured ground truth for the DIS truck and boat (ROADMAP 2.7).

Usage: scripts/gt_occlusion_check.py OUTDIR [--runs main,crest,calm,mloff] [--check-only]
  caffeinate -ims uv run -q --with numpy --with pillow --with pycocotools \\
      python scripts/gt_occlusion_check.py OUTDIR
  (caffeinate: a host that sleeps mid-run freezes CamSim and spoils the frame times)

Four CamSim launches (headless, macOS, local build), each with DIS on, the depth map off,
`send_dis_test.py both` running and the camera driven over CIGI (the vehicles' paths are
deterministic, so the camera predicts where they are):
  main   ground truth on, default sea (Beaufort 3):
         nadir_truck, nadir_boat  - straight down, 10 deg FOV, ~330 m above the vehicle
         edge_truck               - nadir, look-at point offset so the truck sits on the
                                    image's right edge
         terrain_truck            - a fixed camera 20 m above the ground (CIGI HOT), 250 m
                                    outside the loop's east leg, turning with the truck across
                                    Presidio loop (trees / terrain in between)
  crest  ground truth on, CAMSIM_OCEAN_BEAUFORT=6: crest_boat - ~300 m from the boat at
         ~3 deg depression (crests between the camera and the hull)
  calm   the same grazing view at CAMSIM_OCEAN_BEAUFORT=0 (baseline, informational: with the
         submerged-hull cut a flat-sea side view has visibility ~1.0)
  mloff  CAMSIM_ML_ENABLED=0: nadir_truck, nadir_boat again (frame-time baseline)

Each view records the COCO frame-id range it covers (the COCO file is tailed live) and the
wall-clock window (for frames.jsonl). Writes OUTDIR/<run>/ (ml/, frames.jsonl, run.json,
CamSimTest.log), OUTDIR/shots/<view>_<i>.png (/snapshot) and OUTDIR/overlays/<view>_<i>.png
(the shot with that frame's annotations: modal mask magenta, bbox green, amodal bbox orange,
OBB yellow, 3D box cyan). Checks (exit 0 only when 1, 2, 3, 5, 6 pass):
  1. nadir truck / boat: median visibility >= 0.95, max truncation <= 0.01, median OBB
     heading error <= 10 deg (expected angle: the entity's box3d yaw mapped into the image)
  2. edge_truck: some frame with 0.3 <= truncation <= 0.7 whose modal bbox reaches x = W
  3. crest_boat: >= 10 % of boat annotations with visibility < 0.9 (spec Risk 1 if none);
     calm_boat's distribution (expected ~1.0: the hull below the water at the boat is cut from the
     amodal silhouette) and the crest-vs-calm comparison are printed beside it (INFO), or
     an explicit SKIP when OUTDIR has no calm run
  4. terrain_truck: fraction with visibility < 0.9 (informational)
  5. every `segmentation` decodes (pycocotools) and its area equals `area`
  6. median frame time (frames.jsonl wall_ms) over the nadir views, ground truth on vs off:
     |difference| < 2 ms (gpu_ms / render_ms reported alongside)
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import statistics
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
import dis_vehicle_check as dvc
import gt_check_lib as g
import ocean_check as oc
import send_dis_test as sd
from bench import run_bench as rb
from bench import scenario

TRUCK, BOAT = dvc.TRUCK, dvc.BOAT
EDITOR_LOG = Path.home() / "Library/Logs/CamSimTest/CamSimTest.log"
NADIR_UP_M = 330.0
NADIR_FOV = 10.0

VIS_MIN = 0.95
TRUNC_MAX = 0.01
HEADING_MAX_DEG = 10.0
EDGE_TRUNC = (0.3, 0.7)
CREST_VIS = 0.9
CREST_FRACTION = 0.10
CREST_BELOW_CALM = 0.1
TERRAIN_VIS = 0.9
FRAME_DIFF_MS = 2.0


# ---------------------------------------------------------------------------
# Camera poses
# ---------------------------------------------------------------------------


def edge_on(
    preset: sd.Preset, ground_hae: float, up_m: float, fov: float
) -> Callable[[float], scenario.Pose]:
    """Nadir (yaw 0: image +x = east) with the camera shifted west by half the ground
    footprint, so the vehicle's predicted position is on the image's right edge."""
    f = sd.PathFollower(preset.waypoints_ne, preset.speed_mps)
    half_w = up_m * math.tan(math.radians(fov / 2.0))

    def pose(t: float) -> scenario.Pose:
        lat, lon, _ = dvc.vehicle(preset, f, t)
        clat, clon = sd.ne_to_latlon((lat, lon), 0.0, -half_w)
        return scenario.Pose(
            clat, clon, ground_hae + up_m, gimbal_pitch=-90.0, fov_h=fov
        )

    return pose


class FixedLook:
    """A fixed camera `up_m` above the ground at (n_m, e_m) from the preset's centre,
    turning to keep the vehicle's predicted position on the boresight (aimed at
    target_hae). The ground height under the camera is unknown until a CIGI HOT query
    answers (run_once sets `ground_hae`); until then the camera waits at `fallback_hae`."""

    def __init__(
        self,
        preset: sd.Preset,
        n_m: float,
        e_m: float,
        up_m: float,
        target_hae: float,
        fov: float,
        fallback_hae: float = 120.0,
    ) -> None:
        self.preset, self.up_m, self.target_hae, self.fov = (
            preset,
            up_m,
            target_hae,
            fov,
        )
        self.lat, self.lon = sd.ne_to_latlon(preset.center, n_m, e_m)
        self.ground_hae: float | None = None
        self.fallback_hae = fallback_hae
        self.follower = sd.PathFollower(preset.waypoints_ne, preset.speed_mps)

    @property
    def alt(self) -> float:
        return (
            self.fallback_hae
            if self.ground_hae is None
            else self.ground_hae + self.up_m
        )

    def __call__(self, t: float) -> scenario.Pose:
        lat, lon, _ = dvc.vehicle(self.preset, self.follower, t)
        dn = (lat - self.lat) * 111_132.0
        de = (lon - self.lon) * 111_412.0 * math.cos(math.radians(self.lat))
        return scenario.Pose(
            self.lat,
            self.lon,
            self.alt,
            yaw=math.degrees(math.atan2(de, dn)) % 360.0,
            gimbal_pitch=-math.degrees(
                math.atan2(self.alt - self.target_hae, math.hypot(dn, de))
            ),
            fov_h=self.fov,
        )


def query_ground(
    host: rb.Host, responses: oc.Responses, lat: float, lon: float
) -> float | None:
    """Terrain height (HOT, WGS-84 m) at lat/lon over CIGI, or None."""
    for attempt in range(5):
        rid = 200 + attempt
        oc.send_packets(host, oc.pack_hat_hot_request(rid, lat, lon))
        deadline = time.time() + 2.0
        while rid not in responses.hot and time.time() < deadline:
            time.sleep(0.05)
        r = responses.hot.get(rid)
        if r and r.get("valid") and "hot" in r:
            return r["hot"]
        time.sleep(1.0)
    return None


@dataclass
class View:
    name: str
    pose_at: Callable[[float], scenario.Pose]
    cls: str
    window_s: float = 15.0
    shots: int = 3
    settle_s: float = 4.0


@dataclass
class RunSpec:
    label: str
    env: dict[str, str]
    views: list[View] = field(default_factory=list)
    ml: bool = True


def build_runs() -> list[RunSpec]:
    sea = oc.geoid_undulation(*BOAT.center)
    nadir = [
        View(
            "nadir_truck",
            dvc.nadir_on(TRUCK, dvc.TRUCK_GROUND_HAE + NADIR_UP_M, NADIR_FOV),
            "truck",
        ),
        View("nadir_boat", dvc.nadir_on(BOAT, sea + NADIR_UP_M, NADIR_FOV), "boat"),
    ]
    main = nadir + [
        View(
            "edge_truck",
            edge_on(TRUCK, dvc.TRUCK_GROUND_HAE, NADIR_UP_M, NADIR_FOV),
            "truck",
            window_s=25.0,
        ),
        View(
            "terrain_truck",
            # 400 m east of the loop's centre (250 m outside its east leg), 20 m up.
            FixedLook(TRUCK, 0.0, 400.0, 20.0, dvc.TRUCK_GROUND_HAE, 20.0),
            "truck",
            window_s=50.0,
            shots=5,
        ),
    ]
    grazing = dvc.side_on(BOAT, sea, 300.0, 300.0 * math.tan(math.radians(3.0)), 5.0)
    crest = [View("crest_boat", grazing, "boat", window_s=40.0, shots=5)]
    calm = [View("calm_boat", grazing, "boat", window_s=40.0, shots=2)]
    return [
        RunSpec("main", {}, main),
        RunSpec("crest", {"CAMSIM_OCEAN_BEAUFORT": "6"}, crest),
        RunSpec("calm", {"CAMSIM_OCEAN_BEAUFORT": "0"}, calm),
        RunSpec(
            "mloff",
            {"CAMSIM_ML_ENABLED": "0"},
            [View(v.name, v.pose_at, v.cls, v.window_s, 0) for v in nadir],
            ml=False,
        ),
    ]


# ---------------------------------------------------------------------------
# One launch
# ---------------------------------------------------------------------------


def last_frame_id(coco: Path) -> int:
    """frame_id of the last complete COCO line (-1 if none yet). Reads only the tail."""
    if not coco.exists():
        return -1
    with coco.open("rb") as fh:
        fh.seek(0, os.SEEK_END)
        size = fh.tell()
        fh.seek(max(0, size - (1 << 20)))
        tail = fh.read().decode("utf-8", errors="replace")
    for line in reversed(tail.split("\n")):
        if line.startswith('{"frame_id":'):
            try:
                return int(json.loads(line)["frame_id"])
            except (json.JSONDecodeError, KeyError, ValueError):
                continue
    return -1


def run_once(spec: RunSpec, out: Path) -> dict:
    rdir = out / spec.label
    shutil.rmtree(rdir, ignore_errors=True)
    rdir.mkdir(parents=True)
    stats = rdir / "frames.jsonl"
    coco = rdir / "ml" / "camsim_coco.jsonl"
    pid_file = REPO / ".cache" / "camsim.pid"
    if rb.camsim_alive(pid_file):
        sys.exit("A CamSim instance is already running (scripts/stop.sh to stop it)")
    env = dict(
        os.environ,
        CAMSIM_DIS_ENABLED="1",
        CAMSIM_ML_ENABLED="1",
        CAMSIM_ML_OUTPUT_DIR=str(rdir / "ml"),
        CAMSIM_ML_DEPTH_ENABLED="0",
        CAMSIM_ML_INTERVAL_FRAMES="1",
        CAMSIM_SNAPSHOT_ENDPOINT_ENABLED="1",
        CAMSIM_MULTICAST_ADDR="127.0.0.1",
        CAMSIM_FRAME_STATS_PATH=str(stats),
    )
    env.update(spec.env)
    rb.wait_port_free(int(rb.HEALTH.rsplit(":", 1)[1]))
    responses = oc.Responses()
    host = rb.Host()
    host.pose = spec.views[0].pose_at(0.0)  # tiles load where the first view starts
    host.thread.start()  # /ready needs CIGI traffic
    print(f"[gt] {spec.label}: launching ({spec.env or 'defaults'})", flush=True)
    subprocess.run(
        [str(REPO / "scripts" / "run.sh"), "--headless", "--local", "--detach"],
        env=env,
        check=True,
        stdout=subprocess.DEVNULL,
    )
    windows: list[dict] = []
    sender = tracker = None
    try:
        rb.wait_ready(pid_file)
        rb.wait_terrain(120)
        dvc.wait_tiles(stats)
        time.sleep(3.0)
        sender = subprocess.Popen(
            [sys.executable, str(REPO / "scripts" / "send_dis_test.py"), "both"],
            stdout=subprocess.DEVNULL,
        )
        tracker = dvc.Tracker(host, time.monotonic())
        for v in spec.views:
            print(f"[gt] {spec.label}: {v.name}", flush=True)
            if isinstance(v.pose_at, FixedLook):
                look = v.pose_at
                tracker.pose_at = look  # waits at fallback_hae while the tiles load
                time.sleep(2.0)
                dvc.wait_tiles(stats, 60.0)
                look.ground_hae = query_ground(host, responses, look.lat, look.lon)
                print(f"[gt] {v.name}: ground {look.ground_hae} m", flush=True)
            tracker.pose_at = v.pose_at
            time.sleep(2.0)
            dvc.wait_tiles(stats, 60.0)
            time.sleep(v.settle_s)
            w = {
                "name": v.name,
                "ground_hae": getattr(v.pose_at, "ground_hae", None),
                "t0": time.time(),
                "fid0": last_frame_id(coco),
                "shots": [],
            }
            step = v.window_s / max(1, v.shots)
            for i in range(v.shots):
                time.sleep(max(0.0, w["t0"] + (i + 0.5) * step - time.time()))
                png = out / "shots" / f"{v.name}_{i}.png"
                before = last_frame_id(coco)
                if rb.fetch_snapshot(png):
                    w["shots"].append({"png": str(png), "after_fid": before})
            time.sleep(max(0.0, w["t0"] + v.window_s - time.time()))
            w["t1"] = time.time()
            w["fid1"] = last_frame_id(coco)
            windows.append(w)
        time.sleep(1.0)  # let the last frames' COCO lines land
    finally:
        if tracker:
            tracker.stop.set()
        if sender:
            sender.terminate()
        host.stop.set()
        responses.stop.set()
        subprocess.run(
            [str(REPO / "scripts" / "stop.sh")], check=False, stdout=subprocess.DEVNULL
        )
    if EDITOR_LOG.exists():
        shutil.copy(EDITOR_LOG, rdir / "CamSimTest.log")
    result = {"windows": windows}
    (rdir / "run.json").write_text(json.dumps(result, indent=2))
    return result


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------


def view_anns(frames: list[dict], w: dict, cls: str) -> list[tuple[dict, dict]]:
    """(frame, annotation) pairs of class `cls` in the view's COCO frame-id range."""
    lo, hi = w["fid0"], w["fid1"]
    return [(f, a) for f, a in g.ann_for(frames, cls) if lo < f["frame_id"] <= hi]


def write_overlays(out: Path, frames: list[dict], w: dict) -> list[str]:
    """The snapshot is the first frame delivered after the request, and the same frame is
    then annotated: the first COCO frame after the last one written before the request."""
    by_id = sorted(frames, key=lambda f: f["frame_id"])
    made = []
    for s in w["shots"]:
        png = Path(s["png"])
        if not png.exists():
            continue
        frame = next((f for f in by_id if f["frame_id"] > s["after_fid"]), None)
        if frame is None:
            continue
        dst = out / "overlays" / png.name
        g.draw_overlay(png, dst, frame["annotations"])
        made.append(f"{dst.name} (frame {frame['frame_id']})")
    return made


def wall_rows(rdir: Path, w: dict) -> list[dict]:
    rows = [
        json.loads(x)
        for x in (rdir / "frames.jsonl").read_text().splitlines()
        if x.startswith("{")
    ]
    return [r for r in rows if w["t0"] <= r["t"] <= w["t1"] and not r["cut"]]


def med(xs: list[float]) -> float:
    return statistics.median(xs) if xs else float("nan")


def check_all(
    out: Path, results: dict[str, dict]
) -> list[tuple[str, bool | None, str]]:
    lines: list[tuple[str, bool | None, str]] = []
    frames_by_run = {
        label: g.load_coco(out / label / "ml" / "camsim_coco.jsonl")
        for label in results
        if (out / label / "ml" / "camsim_coco.jsonl").exists()
    }
    windows = {
        w["name"] + ("@" + label if label == "mloff" else ""): (label, w)
        for label, r in results.items()
        for w in r["windows"]
    }

    def anns(view: str, cls: str) -> list[tuple[dict, dict]]:
        if view not in windows:
            return []
        label, w = windows[view]
        return view_anns(frames_by_run.get(label, []), w, cls)

    # Overlays.
    for label, r in results.items():
        if label in frames_by_run:
            for w in r["windows"]:
                made = write_overlays(out, frames_by_run[label], w)
                if made:
                    lines.append(
                        ("-", None, f"overlays {w['name']}: " + ", ".join(made))
                    )

    # 1. Nadir.
    for view, cls in (("nadir_truck", "truck"), ("nadir_boat", "boat")):
        p = anns(view, cls)
        if not p:
            lines.append(("1", False, f"{view}: no {cls} annotations"))
            continue
        vis = [a["visibility"] for _, a in p if "visibility" in a]
        trunc = [a["truncation"] for _, a in p if "truncation" in a]
        err = [
            g.obb_heading_error_deg(
                a,
                g.nadir_image_angle_deg(
                    a["box3d"]["yaw_deg"], f["platform"]["yaw_deg"]
                ),
            )
            for f, a in p
            if a.get("obb") and a.get("box3d")
        ]
        render = sum(1 for _, a in p if a.get("mask_source") == "render")
        ok = (
            bool(vis and trunc and err)
            and med(vis) >= VIS_MIN
            and max(trunc) <= TRUNC_MAX
            and med(err) <= HEADING_MAX_DEG
        )
        lines.append(
            (
                "1",
                ok,
                (
                    f"{view}: {len(p)} anns ({render} render); visibility median "
                    f"{med(vis):.3f} min {min(vis, default=float('nan')):.3f} (>= {VIS_MIN}); "
                    f"truncation max {max(trunc, default=float('nan')):.3f} (<= {TRUNC_MAX}); "
                    f"OBB heading error median {med(err):.2f} max "
                    f"{max(err, default=float('nan')):.2f} deg (<= {HEADING_MAX_DEG:g})"
                ),
            )
        )

    # 2. Edge.
    p = anns("edge_truck", "truck")
    trunc = [a["truncation"] for _, a in p if "truncation" in a]
    hits = [
        a
        for _, a in p
        if EDGE_TRUNC[0] <= a.get("truncation", -1) <= EDGE_TRUNC[1]
        and a["bbox"][0] + a["bbox"][2]
        >= a.get("segmentation", {}).get("size", [0, 1280])[1]
    ]
    lines.append(
        (
            "2",
            bool(hits),
            (
                f"edge_truck: {len(p)} anns, truncation range "
                f"{min(trunc, default=float('nan')):.2f}..{max(trunc, default=float('nan')):.2f}; "
                f"{len(hits)} with {EDGE_TRUNC[0]} <= truncation <= {EDGE_TRUNC[1]} and modal "
                "bbox reaching x = W (want >= 1)"
            ),
        )
    )

    # 3. Crest.
    p = anns("crest_boat", "boat")
    vis = [a["visibility"] for _, a in p if "visibility" in a]
    low = sum(1 for v in vis if v < CREST_VIS)
    frac = low / len(vis) if vis else 0.0
    lines.append(
        (
            "3",
            bool(vis) and frac >= CREST_FRACTION,
            (
                f"crest_boat (Beaufort 6): {len(p)} anns; visibility < {CREST_VIS}: {low} "
                f"({100 * frac:.1f} %, want >= {100 * CREST_FRACTION:.0f} %); visibility "
                f"median {med(vis):.3f} min {min(vis, default=float('nan')):.3f}"
            ),
        )
    )

    # 3 (baseline, informational): the same grazing view on a flat sea. Since the submerged-hull
    # cut (final review I2) the hull below the water is not silhouette, so a calm side view
    # is ~1.0 and the brief's bar above is the gate again; the crest-vs-calm line is info.
    p = anns("calm_boat", "boat")
    calm = [a["visibility"] for _, a in p if "visibility" in a]
    if not calm:
        lines.append(
            (
                "3",
                None,
                (
                    "SKIP calm_boat baseline: no calm run (or no boat annotations) in "
                    "this OUTDIR; crest-vs-calm comparison not made"
                ),
            )
        )
    else:
        low = sum(1 for v in calm if v < CREST_VIS)
        lines.append(
            (
                "3",
                None,
                (
                    f"calm_boat (Beaufort 0, same view): {len(p)} anns; visibility < "
                    f"{CREST_VIS}: {low} ({100 * low / len(calm):.1f} %); median "
                    f"{med(calm):.3f} min {min(calm):.3f} max {max(calm):.3f} "
                    "(expect ~1.0: submerged hull cut)"
                ),
            )
        )
        bar = med(calm) - CREST_BELOW_CALM
        low = sum(1 for v in vis if v < bar)
        frac = low / len(vis) if vis else 0.0
        lines.append(
            (
                "3",
                None,
                (
                    f"crest_boat vs calm: visibility < calm median - {CREST_BELOW_CALM:g} "
                    f"({bar:.3f}): {low} ({100 * frac:.1f} %); above the calm max: "
                    f"{sum(1 for v in vis if v > max(calm))} (hull lifted on a crest)"
                ),
            )
        )

    # 4. Terrain (informational).
    p = anns("terrain_truck", "truck")
    vis = [a["visibility"] for _, a in p if "visibility" in a]
    low = sum(1 for v in vis if v < TERRAIN_VIS)
    lines.append(
        (
            "4",
            None,
            (
                f"terrain_truck: {len(p)} anns; visibility < {TERRAIN_VIS}: {low} "
                f"({100 * low / len(vis) if vis else 0:.1f} %); median {med(vis):.3f} "
                f"min {min(vis, default=float('nan')):.3f}"
            ),
        )
    )

    # 5. RLE.
    problems: list[str] = []
    n = 0
    for label, frames in frames_by_run.items():
        n += sum(1 for f in frames for a in f["annotations"] if "segmentation" in a)
        problems += [f"{label}: {x}" for x in g.check_rle(frames)]
    lines.append(
        (
            "5",
            n > 0 and not problems,
            (
                f"segmentation: {n} masks over {len(frames_by_run)} runs, {len(problems)} "
                "problems" + (": " + "; ".join(problems[:5]) if problems else "")
            ),
        )
    )

    # 6. Frame time, nadir views, ground truth on vs off.
    def pooled(suffix: str, key: str) -> list[float]:
        xs: list[float] = []
        for view in ("nadir_truck", "nadir_boat"):
            if view + suffix in windows:
                label, w = windows[view + suffix]
                xs += [r[key] for r in wall_rows(out / label, w)]
        return xs

    on, off = pooled("", "wall_ms"), pooled("@mloff", "wall_ms")
    d = med(on) - med(off)
    lines.append(
        (
            "6",
            bool(on and off) and abs(d) < FRAME_DIFF_MS,
            (
                f"frame time (nadir views, wall_ms median): ground truth on {med(on):.2f} ms "
                f"({len(on)} frames), off {med(off):.2f} ms ({len(off)} frames), diff "
                f"{d:+.2f} ms (< {FRAME_DIFF_MS:g})"
            ),
        )
    )
    for key in ("gpu_ms", "render_ms", "game_ms", "sensor_gpu_ms"):
        a, b = pooled("", key), pooled("@mloff", key)
        if a and b:
            lines.append(
                (
                    "6",
                    None,
                    (
                        f"  {key} median on {med(a):.2f} / off {med(b):.2f} "
                        f"(diff {med(a) - med(b):+.2f})"
                    ),
                )
            )
    return lines


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("outdir", type=Path)
    ap.add_argument(
        "--runs", default="main,crest,calm,mloff", help="comma list of runs"
    )
    ap.add_argument(
        "--check-only", action="store_true", help="re-run the checks on OUTDIR"
    )
    a = ap.parse_args()
    out = a.outdir.resolve()
    (out / "shots").mkdir(parents=True, exist_ok=True)
    wanted = set(filter(None, a.runs.split(",")))
    results: dict[str, dict] = {}
    for spec in build_runs():
        if spec.label in wanted and not a.check_only:
            results[spec.label] = run_once(spec, out)
        elif (out / spec.label / "run.json").exists():
            results[spec.label] = json.loads(
                (out / spec.label / "run.json").read_text()
            )

    lines = check_all(out, results)
    report = []
    for check, ok, text in lines:
        tag = "INFO" if ok is None else ("PASS" if ok else "FAIL")
        if ok is None and text.startswith("SKIP "):
            tag, text = "SKIP", text[len("SKIP ") :]
        report.append(f"[{check}] {tag} {text}")
    text = "\n".join(report)
    (out / "summary.txt").write_text(text + "\n")
    print(text)
    gating = [ok for check, ok, _ in lines if check in {"1", "2", "3", "5", "6"}]
    passed = all(ok is not False for ok in gating) and {"1", "2", "3", "5", "6"} <= {
        c for c, ok, _ in lines if ok is not None
    }
    print(f"Overlays in {out / 'overlays'}; {'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
