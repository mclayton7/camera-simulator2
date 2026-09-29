#!/usr/bin/env python3
"""End-to-end check: DIS truck + boat -> video + COCO ground truth (macOS, local build).

Usage: scripts/dis_vehicle_check.py OUTDIR
  uv run -q --with numpy --with pillow python scripts/dis_vehicle_check.py OUTDIR

Launches CamSim headless with DIS and ground truth on, runs scripts/send_dis_test.py
(`both`) and frames each vehicle over CIGI: a wide nadir view of its path, then close-ups
that follow it (the sender's path is deterministic, so the camera predicts where the
vehicle is). Writes OUTDIR/shots/*.png, OUTDIR/ml/ (COCO), OUTDIR/frames.jsonl and
prints a summary. Exit 0 when COCO has exactly one stable entity_id each for `truck`
and `boat`; the shots are for a human to review (facing, on the ground / water, tilt).
"""

from __future__ import annotations

import json
import math
import os
import subprocess
import sys
import threading
import time
from collections.abc import Callable
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
import send_dis_test as sd
from bench import run_bench as rb
from bench import scenario

TRUCK = sd.PRESETS["truck-loop"]
BOAT = sd.PRESETS["boat-circle"]
HITCH_MS = 66.0  # two frames at 30 fps
# Rough surface heights (WGS-84 ellipsoid) for aiming the oblique close-ups: the
# Presidio loop spans about -5..47 m (0..15 m on the east and south legs), the
# bay's rendered surface about -55 m (bathymetry, see docs/dis.md). The
# 12 deg FOV at ~250 m tolerates about +-20 m of error. Close-ups are slow to
# render (fine tiles stream in: frames of 100+ ms), wide views run at 30 fps.
TRUCK_GROUND_HAE = 12.0
BOAT_SURFACE_HAE = -50.0


class Tracker:
    """Moves the CIGI host's pose every 1/60 s from a function of sender time."""

    def __init__(self, host: rb.Host, t0: float) -> None:
        self.host, self.t0 = host, t0
        self.pose_at: Callable[[float], scenario.Pose] | None = None
        self.stop = threading.Event()
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self) -> None:
        while not self.stop.is_set():
            if self.pose_at:
                self.host.pose = self.pose_at(time.monotonic() - self.t0)
            time.sleep(1.0 / 60.0)


def vehicle(preset: sd.Preset, follower: sd.PathFollower, t: float) -> tuple[float, float, float]:
    n, e, heading, _ = follower.state(t)
    lat, lon = sd.ne_to_latlon(preset.center, n, e)
    return lat, lon, heading


def nadir_on(preset: sd.Preset, alt: float, fov: float) -> Callable[[float], scenario.Pose]:
    f = sd.PathFollower(preset.waypoints_ne, preset.speed_mps)

    def pose(t: float) -> scenario.Pose:
        lat, lon, _ = vehicle(preset, f, t)
        return scenario.Pose(lat, lon, alt, gimbal_pitch=-90.0, fov_h=fov)

    return pose


def side_on(preset: sd.Preset, surface_hae: float) -> Callable[[float], scenario.Pose]:
    """From the vehicle's left, 160 m out and 190 m up (50 deg down): nose to the left."""
    f = sd.PathFollower(preset.waypoints_ne, preset.speed_mps)
    out_m, up_m = 160.0, 190.0

    def pose(t: float) -> scenario.Pose:
        lat, lon, h = vehicle(preset, f, t)
        left = math.radians(h - 90.0)
        clat, clon = sd.ne_to_latlon((lat, lon), out_m * math.cos(left), out_m * math.sin(left))
        return scenario.Pose(clat, clon, surface_hae + up_m, yaw=(h + 90.0) % 360.0,
                             gimbal_pitch=-math.degrees(math.atan2(up_m, out_m)), fov_h=12.0)

    return pose


def wait_tiles(stats: Path, timeout_s: float = 90.0) -> None:
    """Wait until the frame stats report the view fully loaded, so the vehicles'
    arrival is timed on a settled stream rather than during tile streaming."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        lines = stats.read_text().splitlines()[-1:] if stats.exists() else []
        if lines and lines[0].startswith("{") and json.loads(lines[0])["load_pct"] >= 99.9:
            return
        time.sleep(1.0)
    print(f"[check] tiles not fully loaded after {timeout_s:.0f}s; carrying on")


def shoot(out: Path, name: str, n: int = 1, every_s: float = 4.0) -> None:
    for i in range(n):
        rb.fetch_snapshot(out / "shots" / (f"{name}_{i}.png" if n > 1 else f"{name}.png"))
        if i + 1 < n:
            time.sleep(every_s)


def main() -> int:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1]).resolve()
    (out / "shots").mkdir(parents=True, exist_ok=True)
    pid_file = REPO / ".cache" / "camsim.pid"
    if rb.camsim_alive(pid_file):
        sys.exit("A CamSim instance is already running (scripts/stop.sh to stop it)")
    env = dict(
        os.environ,
        CAMSIM_DIS_ENABLED="1",
        CAMSIM_ML_ENABLED="1",
        CAMSIM_ML_OUTPUT_DIR=str(out / "ml"),
        CAMSIM_ML_DEPTH_ENABLED="0",
        CAMSIM_ML_INTERVAL_FRAMES="3",
        CAMSIM_SNAPSHOT_ENDPOINT_ENABLED="1",
        CAMSIM_MULTICAST_ADDR="127.0.0.1",
        CAMSIM_FRAME_STATS_PATH=str(out / "frames.jsonl"),
    )
    rb.wait_port_free(int(rb.HEALTH.rsplit(":", 1)[1]))
    host = rb.Host()
    # Nadir over the truck loop, 30 deg FOV: ~430 m of ground across at ~850 m above it.
    host.pose = scenario.Pose(TRUCK.center[0], TRUCK.center[1], 900.0, gimbal_pitch=-90.0, fov_h=30.0)
    host.thread.start()  # /ready needs CIGI traffic
    subprocess.run(
        [str(REPO / "scripts" / "run.sh"), "--headless", "--local", "--detach"],
        env=env,
        check=True,
        stdout=subprocess.DEVNULL,
    )
    sender = tracker = None
    sender_wall = 0.0
    try:
        rb.wait_ready(pid_file)
        rb.wait_terrain(120)
        wait_tiles(out / "frames.jsonl")
        time.sleep(3.0)
        sender_wall = time.time()
        sender = subprocess.Popen(
            [sys.executable, str(REPO / "scripts" / "send_dis_test.py"), "both"],
            stdout=subprocess.DEVNULL,
        )
        tracker = Tracker(host, time.monotonic())
        time.sleep(10.0)
        shoot(out, "truck_wide")
        tracker.pose_at = nadir_on(TRUCK, TRUCK_GROUND_HAE + 330.0, 10.0)  # ~58 m across
        time.sleep(6.0)
        shoot(out, "truck_nadir", 3)
        tracker.pose_at = side_on(TRUCK, TRUCK_GROUND_HAE)
        time.sleep(6.0)
        shoot(out, "truck_side", 4)
        tracker.pose_at = None
        host.pose = scenario.Pose(BOAT.center[0], BOAT.center[1], 800.0, gimbal_pitch=-90.0, fov_h=30.0)
        time.sleep(8.0)
        shoot(out, "boat_wide")
        tracker.pose_at = nadir_on(BOAT, BOAT_SURFACE_HAE + 330.0, 10.0)
        time.sleep(6.0)
        shoot(out, "boat_nadir", 3)
        tracker.pose_at = side_on(BOAT, BOAT_SURFACE_HAE)
        time.sleep(6.0)
        shoot(out, "boat_side", 3)
    finally:
        if tracker:
            tracker.stop.set()
        if sender:
            sender.terminate()
        host.stop.set()
        subprocess.run(
            [str(REPO / "scripts" / "stop.sh")], check=False, stdout=subprocess.DEVNULL
        )

    return summarize(out, sender_wall)


def summarize(out: Path, sender_wall: float = 0.0) -> int:
    ids: dict[str, set[int]] = {}
    frames = 0
    for f in (out / "ml").rglob("*.jsonl"):
        for line in f.read_text().splitlines():
            if not line.startswith("{"):
                continue
            rec = json.loads(line)
            frames += 1
            for a in rec.get("annotations", []):
                ids.setdefault(a["category"]["name"], set()).add(a["entity_id"])
    stats = out / "frames.jsonl"
    rows = (
        [json.loads(x) for x in stats.read_text().splitlines() if x.startswith("{")]
        if stats.exists()
        else []
    )
    walls = sorted(r["wall_ms"] for r in rows) or [0.0]
    hitches = sum(1 for r in rows if r["wall_ms"] > HITCH_MS)
    p95 = walls[int(0.95 * (len(walls) - 1))]
    print(f"COCO frames {frames}; ids per class {ids}")
    print(f"frames {len(rows)}; wall p95 {p95:.1f} ms; frames > {HITCH_MS:.0f} ms: {hitches} "
          "(camera cuts and tile streaming included)")
    if sender_wall:
        # The vehicles spawn within the first second of the sender; the camera holds still.
        spawn = [r["wall_ms"] for r in rows if sender_wall <= r["t"] < sender_wall + 5.0]
        if spawn:
            print(f"first 5 s of DIS: {len(spawn)} frames, max {max(spawn):.1f} ms, "
                  f"> {HITCH_MS:.0f} ms: {sum(1 for w in spawn if w > HITCH_MS)}")
    ok = bool(ids.get("truck") and ids.get("boat")) and all(len(v) == 1 for v in ids.values())
    print("PASS (labels): one stable id each for truck and boat" if ok else "FAIL (labels)")
    print(f"Review the shots in {out / 'shots'}: on the ground / water, facing the travel "
          "direction (side views: nose to the left).")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
