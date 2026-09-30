#!/usr/bin/env python3
"""Ocean acceptance check: a DIS boat on the sea at several sea states (ROADMAP 2.6).

Usage: scripts/ocean_check.py OUTDIR [--beaufort 0,3,6] [--cigi]
  caffeinate -ims uv run -q --with numpy --with pillow python scripts/ocean_check.py OUTDIR --cigi
  (caffeinate: a host that sleeps mid-run freezes CamSim and spoils the frame times)
  --check-only re-runs the checks on OUTDIR; --views NAME,.. and --window-s S run a subset
  of views with a longer measured window (diagnostics; set CAMSIM_OCEAN_ENABLED=0 to compare).

For each sea state, launches CamSim headless (macOS, local build) with
CAMSIM_OCEAN_BEAUFORT=<b>, DIS and COCO ground truth on, runs
`send_dis_test.py boat-circle` and drives the camera over CIGI through a fixed
set of views (the boat's path is deterministic, so the camera predicts where it
is). `--cigi` adds one run whose sea comes from two CIGI Wave Control packets
(opcode 14) instead of the Beaufort table. Every run also sends one extended
HAT/HOT request (opcode 24) at the boat and reads the response (opcode 103).

Writes OUTDIR/shots/<run>_<view>.png and OUTDIR/<run>/ (ml/, frames.jsonl,
stream.ts, CamSimTest.log). Prints PASS/FAIL per check and exits non-zero on
any FAIL:
  - terrain: the Cesium tiles loaded (>= 99%) during the run (else nothing else means much)
  - COCO: exactly one `boat` entity_id across all frames
  - boat altitude (COCO `geo.alt_m`) within 0.5 m + a_max of EGM96 sea level
  - HOT at the boat within 0.5 m + a_max of sea level
  - frame times over the settled windows: median <= 33.4 ms (one 30 fps frame), none > 66 ms
  - "Ocean: mesh rebuilt in X ms": reported against 10 ms, FAIL only over 30 ms
a_max = 1.5 * Hs / 2 for a Beaufort sea (Hs from the table), sum of the wave
amplitudes for the CIGI sea.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
import dis_vehicle_check as dvc
import send_cigi_test as sc
import send_dis_test as sd
from bench import run_bench as rb
from bench import scenario

BOAT = sd.PRESETS["boat-circle"]
GEOID_DAC = REPO / "unreal_project/CamSimTest/Content/NonUFS/Geoid/WW15MGH.DAC"
EDITOR_LOG = Path.home() / "Library/Logs/CamSimTest/CamSimTest.log"
RESPONSE_PORT = int(os.environ.get("CAMSIM_CIGI_RESPONSE_PORT", "8889"))
STREAM_PORT = int(os.environ.get("CAMSIM_MULTICAST_PORT", "5004"))

# Significant wave height (m) per Beaufort number: Ocean/FBeaufortTable.h.
BEAUFORT_HS = [0.0, 0.1, 0.3, 0.6, 1.0, 1.5, 2.5, 4.0, 5.0, 7.0, 9.0, 11.5, 14.0]
# The CIGI run's sea: (height crest-to-trough m, wavelength m, direction TOWARD deg).
CIGI_WAVES = [(1.5, 45.0, 60.0), (0.8, 20.0, 100.0)]
CIGI_BASE_BEAUFORT = 3.0  # replaced by the Wave Control set

ALT_TOL_M = 0.5
FRAME_MEDIAN_MS = (
    1000.0 / 30.0 + 0.1
)  # "33 ms" = one frame at the locked 30 fps (33.33 ms)
FRAME_MAX_MS = 66.0
REBUILD_BUDGET_MS = 10.0
REBUILD_FAIL_MS = 30.0

# Golden Gate Bridge (mid-span) and the shallow cove at Aquatic Park.
GOLDEN_GATE = (37.8199, -122.4783)
SHALLOWS = (37.8078, -122.4232)


# ---------------------------------------------------------------------------
# EGM96 sea level (the same 15' grid and bilinear interpolation as
# Geospatial/Geoid.cpp and scripts/klv_conformance/check.js)
# ---------------------------------------------------------------------------

_GEOID: bytes | None = None


def geoid_undulation(lat: float, lon: float) -> float:
    global _GEOID
    if _GEOID is None:
        _GEOID = GEOID_DAC.read_bytes()
        if len(_GEOID) != 721 * 1440 * 2:
            sys.exit(f"{GEOID_DAC} is not the EGM96 grid (git lfs pull?)")

    def at(row: int, col: int) -> float:
        i = 2 * (row * 1440 + col % 1440)
        return struct.unpack_from(">h", _GEOID, i)[0] / 100.0

    y = (90.0 - lat) / 0.25
    x = (lon % 360.0) / 0.25
    r, c = min(math.floor(y), 719), math.floor(x)
    fy, fx = y - r, x - c
    return (at(r, c) * (1 - fx) + at(r, c + 1) * fx) * (1 - fy) + (
        at(r + 1, c) * (1 - fx) + at(r + 1, c + 1) * fx
    ) * fy


# ---------------------------------------------------------------------------
# CIGI 3.3 packets the harness doesn't have (CCL-free, big-endian like
# send_cigi_test.py; the IG Control byte-swap magic declares the order)
# ---------------------------------------------------------------------------


def pack_wave_control(
    wave_id: int,
    height_m: float,
    length_m: float,
    period_s: float,
    toward_deg: float,
    enable: bool = True,
    phase_deg: float = 0.0,
) -> bytes:
    """Wave Control, opcode 14, 32 bytes (CIGI 3.3 ICD 4.1.14), Global scope.

    0 ID, 1 size, 2-3 Entity/Region ID, 4 Wave ID, 5 Wave Enable (bit 0) |
    Scope (bits 1-2, 0 = Global) | Breaker Type (bits 3-4), 6-7 reserved,
    8 Wave Height (m, crest to trough), 12 Wavelength (m), 16 Period (s),
    20 Direction (deg, the direction the wave propagates toward), 24 Phase
    Offset (deg), 28 Leading (deg)."""
    return struct.pack(
        ">BBHBBHffffff",
        14,
        32,
        0,
        wave_id,
        1 if enable else 0,
        0,
        height_m,
        length_m,
        period_s,
        toward_deg,
        phase_deg,
        0.0,
    )


def pack_hat_hot_request(
    request_id: int, lat: float, lon: float, alt: float = 0.0, req_type: int = 2
) -> bytes:
    """HAT/HOT Request, opcode 24, 32 bytes (CIGI 3.3 ICD 4.1.24), geodetic.

    0 ID, 1 size, 2-3 HAT/HOT ID, 4 Request Type (bits 0-1: 0 HAT, 1 HOT,
    2 extended) | Coordinate System (bit 2, 0 = geodetic), 5 Update Period
    (0 = one shot), 6-7 Entity ID, 8 Lat, 16 Lon, 24 Alt (doubles)."""
    return struct.pack(
        ">BBHBBHddd", 24, 32, request_id, req_type & 0x3, 0, 0, lat, lon, alt
    )


def deep_water_period(length_m: float) -> float:
    return math.sqrt(2.0 * math.pi * length_m / 9.80665)


class Responses:
    """Collects IG -> host HAT/HOT responses (opcodes 102 / 103) by ID."""

    def __init__(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", RESPONSE_PORT))
        self.sock.settimeout(0.5)
        self.hot: dict[int, dict] = {}
        self.stop = threading.Event()
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self) -> None:
        while not self.stop.is_set():
            try:
                data, _ = self.sock.recvfrom(65535)
            except (TimeoutError, OSError):
                continue
            self._parse(data)
        self.sock.close()

    def _parse(self, data: bytes) -> None:
        # CCL writes in the IG's native order; SOF (first packet) carries the
        # byte-swap magic 0x8000 at bytes 6-7.
        e = ">" if data[:1] == b"\x65" and data[6:8] == b"\x80\x00" else "<"
        i = 0
        while i + 2 <= len(data):
            op, size = data[i], data[i + 1]
            if size < 2 or i + size > len(data):
                break
            p = data[i : i + size]
            if op == 102 and size >= 16:  # HAT/HOT Response
                (rid,) = struct.unpack_from(e + "H", p, 2)
                (h,) = struct.unpack_from(e + "d", p, 8)
                kind = "hot" if p[4] & 0x02 else "hat"
                self.hot[rid] = {"valid": bool(p[4] & 1), kind: h}
            elif op == 103 and size >= 40:  # HAT/HOT Extended Response
                (rid,) = struct.unpack_from(e + "H", p, 2)
                hat, hot = struct.unpack_from(e + "dd", p, 8)
                (mat,) = struct.unpack_from(e + "I", p, 24)
                az, el = struct.unpack_from(e + "ff", p, 28)
                self.hot[rid] = {
                    "valid": bool(p[4] & 1),
                    "hat": hat,
                    "hot": hot,
                    "material": mat,
                    "normal_az": az,
                    "normal_el": el,
                }
            i += size


def send_packets(host: rb.Host, payload: bytes) -> None:
    """One extra host datagram (IG Control first, as CIGI requires)."""
    host.sock.sendto(
        sc.pack_ig_control(host.frame) + payload, ("127.0.0.1", rb.CIGI_PORT)
    )


# ---------------------------------------------------------------------------
# Views
# ---------------------------------------------------------------------------


@dataclass
class View:
    name: str
    pose_at: Callable[[float], scenario.Pose] | scenario.Pose
    settle_s: float = 4.0  # after the tiles report loaded
    window_s: float = 6.0  # measured frame-time window
    boat: bool = True  # the boat is in frame


def bearing_deg(a: tuple[float, float], b: tuple[float, float]) -> float:
    dn = (b[0] - a[0]) * 111_132.0
    de = (b[1] - a[1]) * 111_412.0 * math.cos(math.radians(a[0]))
    return math.degrees(math.atan2(de, dn)) % 360.0


def build_views(sea: float) -> list[View]:
    gg_cam = (37.8110, -122.4520)
    return [
        # Nadir over the boat circle (300 m across) from 1000 m: ~1150 m of sea across.
        View(
            "nadir_wide",
            scenario.Pose(
                BOAT.center[0],
                BOAT.center[1],
                sea + 1000.0,
                gimbal_pitch=-90.0,
                fov_h=60.0,
            ),
        ),
        # 12 deg close-up following the boat, 160 m out and 190 m up (~250 m slant).
        View("oblique_close", dvc.side_on(BOAT, sea)),
        # 500 ft above the water, 500 ft out: 45 deg down, boresight on the boat.
        View("boat_500ft_45deg", dvc.side_on(BOAT, sea, 152.4, 152.4, 30.0)),
        View(
            "boat_500ft_45deg_fov60",
            dvc.side_on(BOAT, sea, 152.4, 152.4, 60.0),
            2.0,
            3.0,
        ),
        # Waterline: 6 m above sea level, 40 m from the boat.
        View("waterline", dvc.side_on(BOAT, sea, 40.0, 6.0, 20.0)),
        # Shallow cove (seabed through the water), nadir from 300 m.
        View(
            "shallows",
            scenario.Pose(
                SHALLOWS[0], SHALLOWS[1], sea + 300.0, gimbal_pitch=-90.0, fov_h=60.0
            ),
            boat=False,
        ),
        # Golden Gate coastline, low oblique from over the bay.
        View(
            "coastline_golden_gate",
            scenario.Pose(
                gg_cam[0],
                gg_cam[1],
                sea + 300.0,
                yaw=bearing_deg(gg_cam, GOLDEN_GATE),
                gimbal_pitch=-6.0,
                fov_h=60.0,
            ),
            boat=False,
        ),
        # 10 km up, looking west over the Pacific (horizon dip ~3.2 deg).
        View(
            "horizon_10km",
            scenario.Pose(
                37.80, -122.50, 10_000.0, yaw=270.0, gimbal_pitch=-6.0, fov_h=60.0
            ),
            settle_s=6.0,
            boat=False,
        ),
    ]


# ---------------------------------------------------------------------------
# One run
# ---------------------------------------------------------------------------


@dataclass
class Run:
    label: str
    beaufort: float
    cigi: bool

    @property
    def a_max(self) -> float:
        if self.cigi:
            return sum(h / 2.0 for h, _, _ in CIGI_WAVES)
        b = max(0.0, min(12.0, self.beaufort))
        lo = math.floor(b)
        hi = min(lo + 1, 12)
        hs = BEAUFORT_HS[lo] + (BEAUFORT_HS[hi] - BEAUFORT_HS[lo]) * (b - lo)
        return 1.5 * hs / 2.0


def run_once(
    run: Run, out: Path, only: set[str] | None = None, window_s: float | None = None
) -> dict:
    rdir = out / run.label
    shutil.rmtree(rdir, ignore_errors=True)
    rdir.mkdir(parents=True)
    stats = rdir / "frames.jsonl"
    pid_file = REPO / ".cache" / "camsim.pid"
    if rb.camsim_alive(pid_file):
        sys.exit("A CamSim instance is already running (scripts/stop.sh to stop it)")
    env = dict(
        os.environ,
        CAMSIM_OCEAN_BEAUFORT=str(run.beaufort),
        CAMSIM_DIS_ENABLED="1",
        CAMSIM_ML_ENABLED="1",
        CAMSIM_ML_OUTPUT_DIR=str(rdir / "ml"),
        CAMSIM_ML_DEPTH_ENABLED="0",
        CAMSIM_ML_INTERVAL_FRAMES="3",
        CAMSIM_SNAPSHOT_ENDPOINT_ENABLED="1",
        CAMSIM_MULTICAST_ADDR="127.0.0.1",
        CAMSIM_FRAME_STATS_PATH=str(stats),
    )
    sea = geoid_undulation(*BOAT.center)
    views = [v for v in build_views(sea) if not only or v.name in only]
    for v in views:
        if window_s:
            v.window_s = window_s
    rb.wait_port_free(int(rb.HEALTH.rsplit(":", 1)[1]))
    responses = Responses()
    host = rb.Host()
    host.pose = build_views(sea)[0].pose_at  # the static nadir: tiles load there first
    host.thread.start()  # /ready needs CIGI traffic
    print(f"[ocean] {run.label}: launching (Beaufort {run.beaufort})", flush=True)
    subprocess.run(
        [str(REPO / "scripts" / "run.sh"), "--headless", "--local", "--detach"],
        env=env,
        check=True,
        stdout=subprocess.DEVNULL,
    )
    windows: list[tuple[str, float, float]] = []
    hot: dict = {}
    sender = tracker = None
    try:
        rb.wait_ready(pid_file)
        rb.wait_terrain(120)
        if run.cigi:
            waves = b"".join(
                pack_wave_control(i, h, lam, deep_water_period(lam), toward)
                for i, (h, lam, toward) in enumerate(CIGI_WAVES)
            )
            for _ in range(3):
                send_packets(host, waves)
                time.sleep(0.1)
        dvc.wait_tiles(stats)
        time.sleep(3.0)
        sender = subprocess.Popen(
            [sys.executable, str(REPO / "scripts" / "send_dis_test.py"), "boat-circle"],
            stdout=subprocess.DEVNULL,
        )
        tracker = dvc.Tracker(host, time.monotonic())
        follower = sd.PathFollower(BOAT.waypoints_ne, BOAT.speed_mps)
        for v in views:
            print(f"[ocean] {run.label}: {v.name}", flush=True)
            if callable(v.pose_at):
                tracker.pose_at = v.pose_at
            else:
                tracker.pose_at = None
                host.pose = v.pose_at
            time.sleep(2.0)
            dvc.wait_tiles(stats, 60.0)
            time.sleep(v.settle_s)
            start = time.time()
            ts = None
            if v.name == "nadir_wide":
                ts = subprocess.Popen(
                    [
                        "ffmpeg",
                        "-y",
                        "-loglevel",
                        "error",
                        "-i",
                        f"udp://127.0.0.1:{STREAM_PORT}?timeout=5000000",
                        "-t",
                        str(v.window_s),
                        "-map",
                        "0",  # video and the KLV data stream
                        "-c",
                        "copy",
                        str(rdir / "stream.ts"),
                    ],
                    stderr=subprocess.DEVNULL,  # joins mid-GOP: decoder noise until an IDR
                )
                # HOT at the boat's predicted position, now.
                for attempt in range(3):
                    rid = 100 + attempt
                    lat, lon, _ = dvc.vehicle(
                        BOAT, follower, time.monotonic() - tracker.t0
                    )
                    send_packets(host, pack_hat_hot_request(rid, lat, lon))
                    deadline = time.time() + 2.0
                    while rid not in responses.hot and time.time() < deadline:
                        time.sleep(0.05)
                    if rid in responses.hot:
                        hot = dict(responses.hot[rid], lat=lat, lon=lon)
                        break
            time.sleep(max(0.0, v.window_s - (time.time() - start)))
            windows.append((v.name, start, time.time()))
            dvc.shoot(out, f"{run.label}_{v.name}")
            if ts:
                try:
                    ts.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    ts.kill()
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
    result = {"windows": windows, "hot": hot, "sea_at_centre": sea}
    (rdir / "run.json").write_text(json.dumps(result, indent=2))
    return result


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------


def check_run(run: Run, out: Path, result: dict) -> list[tuple[bool | None, str]]:
    rdir = out / run.label
    tol = ALT_TOL_M + run.a_max
    lines: list[tuple[bool | None, str]] = []

    rows = [
        json.loads(x)
        for x in (rdir / "frames.jsonl").read_text().splitlines()
        if x.startswith("{")
    ]
    # Terrain: without Cesium tiles (e.g. no network) the sea covers an empty
    # world and every other check is meaningless.
    peak = max((r["load_pct"] for r in rows), default=0.0)
    lines.append(
        (peak >= 99.0, f"terrain tiles loaded: peak {peak:.1f}% (want >= 99%)")
    )

    # Boat in frame in every boat view. COCO timestamps are sim time (the harness
    # sets the date over CIGI), so records are matched to views by the camera
    # altitude they carry (views that share an altitude are checked together).
    by_alt: dict[float, list[str]] = {}
    for v in build_views(result["sea_at_centre"]):
        if v.boat:
            pose = v.pose_at(0.0) if callable(v.pose_at) else v.pose_at
            by_alt.setdefault(round(pose.alt, 1), []).append(v.name)
    seen = dict.fromkeys(by_alt, 0)
    for f in (rdir / "ml").rglob("*.jsonl"):
        for line in f.read_text().splitlines():
            if not line.startswith("{"):
                continue
            rec = json.loads(line)
            if not any(a["category"]["name"] == "boat" for a in rec["annotations"]):
                continue
            for alt in seen:
                if abs(rec["platform"]["alt_m"] - alt) < 2.0:
                    seen[alt] += 1
    lines.append(
        (
            all(seen.values()),
            "boat labelled in every boat view: "
            + "; ".join(f"{'+'.join(by_alt[a])} {n}" for a, n in seen.items()),
        )
    )

    # COCO: one stable boat id; altitude vs EGM96 sea level.
    ids: set[int] = set()
    errs: list[float] = []
    frames = 0
    for f in (rdir / "ml").rglob("*.jsonl"):
        for line in f.read_text().splitlines():
            if not line.startswith("{"):
                continue
            frames += 1
            for a in json.loads(line).get("annotations", []):
                if a["category"]["name"] != "boat":
                    continue
                ids.add(a["entity_id"])
                g = a.get("geo")
                if g:
                    errs.append(g["alt_m"] - geoid_undulation(g["lat"], g["lon"]))
    lines.append(
        (
            len(ids) == 1,
            f"COCO: {frames} frames, boat ids {sorted(ids)} (want exactly one)",
        )
    )
    if errs:
        worst = max(errs, key=abs)
        lines.append(
            (
                abs(worst) <= tol,
                (
                    f"boat altitude - sea level over {len(errs)} labels: mean {statistics.fmean(errs):+.2f} m, "
                    f"range {min(errs):+.2f}..{max(errs):+.2f} m, sd {statistics.pstdev(errs):.2f} m "
                    f"(limit +-{tol:.2f} m)"
                ),
            )
        )
    else:
        lines.append((False, "boat altitude: no COCO labels with geo"))

    # HOT at the boat.
    hot = result.get("hot") or {}
    if hot.get("valid") and "hot" in hot:
        sea = geoid_undulation(hot["lat"], hot["lon"])
        d = hot["hot"] - sea
        extra = (
            f", normal az {hot['normal_az']:.1f} el {hot['normal_el']:.1f} deg"
            if "normal_el" in hot
            else ""
        )
        lines.append(
            (
                abs(d) <= tol,
                (
                    f"HOT at the boat {hot['hot']:.2f} m, sea level {sea:.2f} m, diff {d:+.2f} m "
                    f"(limit +-{tol:.2f} m){extra}"
                ),
            )
        )
    else:
        lines.append((False, f"HOT at the boat: no valid response ({hot or 'none'})"))

    # Frame times over the settled windows.
    all_ms: list[float] = []
    per_view = []
    for name, t0, t1 in result["windows"]:
        ms = [r["wall_ms"] for r in rows if t0 <= r["t"] <= t1 and not r["cut"]]
        gpu = [r["gpu_ms"] for r in rows if t0 <= r["t"] <= t1 and not r["cut"]]
        if ms:
            all_ms += ms
            per_view.append(
                f"{name} {statistics.median(ms):.1f}/{max(ms):.0f}"
                f" (gpu {statistics.median(gpu):.1f})"
            )
    if all_ms:
        med, mx = statistics.median(all_ms), max(all_ms)
        over = sum(1 for m in all_ms if m > FRAME_MAX_MS)
        lines.append(
            (
                med <= FRAME_MEDIAN_MS and over == 0,
                (
                    f"frame times ({len(all_ms)} settled frames): median {med:.1f} ms, max {mx:.1f} ms, "
                    f"> {FRAME_MAX_MS:.0f} ms: {over}"
                ),
            )
        )
        lines.append((None, "  per view median/max ms: " + "; ".join(per_view)))
    else:
        lines.append((False, "frame times: no rows in the settled windows"))

    # Mesh rebuild CPU time.
    log = rdir / "CamSimTest.log"
    builds = (
        [
            float(m)
            for m in re.findall(
                r"Ocean: mesh rebuilt in ([\d.]+) ms", log.read_text(errors="replace")
            )
        ]
        if log.exists()
        else []
    )
    if builds:
        mx = max(builds)
        lines.append(
            (
                mx <= REBUILD_FAIL_MS,
                (
                    f"mesh rebuilds: {len(builds)}, median {statistics.median(builds):.1f} ms, "
                    f"max {mx:.1f} ms ({'within' if mx <= REBUILD_BUDGET_MS else 'OVER'} the "
                    f"{REBUILD_BUDGET_MS:.0f} ms budget; FAIL over {REBUILD_FAIL_MS:.0f} ms)"
                ),
            )
        )
    else:
        lines.append(
            (False, "mesh rebuilds: no 'Ocean: mesh rebuilt' lines in the log")
        )
    return lines


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("outdir", type=Path)
    ap.add_argument("--beaufort", default="0,3,6", help="comma list ('' for none)")
    ap.add_argument("--cigi", action="store_true", help="add the Wave Control run")
    ap.add_argument(
        "--views", default="", help="only these views (comma list; diagnostics)"
    )
    ap.add_argument(
        "--window-s", type=float, default=None, help="measured window per view"
    )
    ap.add_argument(
        "--check-only",
        action="store_true",
        help="re-run the checks on an existing OUTDIR",
    )
    a = ap.parse_args()
    out = a.outdir.resolve()
    (out / "shots").mkdir(parents=True, exist_ok=True)
    runs = [
        Run(f"b{b:g}", b, False) for b in (float(x) for x in a.beaufort.split(",") if x)
    ]
    if a.cigi:
        runs.append(Run("cigi", CIGI_BASE_BEAUFORT, True))

    report: list[str] = []
    failed = 0
    for run in runs:
        if a.check_only:
            result = json.loads((out / run.label / "run.json").read_text())
        else:
            result = run_once(
                run, out, set(filter(None, a.views.split(","))), a.window_s
            )
        report.append(
            f"== {run.label} (a_max {run.a_max:.2f} m"
            + (", CIGI Wave Control" if run.cigi else f", Beaufort {run.beaufort:g}")
            + ")"
        )
        for ok, text in check_run(run, out, result):
            failed += ok is False
            tag = "INFO" if ok is None else ("PASS" if ok else "FAIL")
            report.append(f"{tag} {text}")
    text = "\n".join(report)
    (out / "summary.txt").write_text(text + "\n")
    print(text)
    print(
        f"Shots in {out / 'shots'}; {'all checks passed' if not failed else f'{failed} FAIL'}"
    )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
