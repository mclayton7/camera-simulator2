#!/usr/bin/env python3
"""REALISM R6 check 2, determinism of settled stills (spec docs/superpowers/specs/2026-10-15-r6-georeg-determinism-design.md
section 2): the same package, pins and CIGI input give the same frames.

    TMPDIR=/opt/mac/tmp uv run --project scripts/scene python scripts/determinism_check.py \\
        --scene .cache/scene-packages/pendleton-r5veg --out OUT [--against OTHER_OUT] [--check-only]

Two launches (a, b) fly the same six poses in the same order: camp_nadir, trees_oblique, slant_5km, coast_waves (EO)
and camp_nadir_ir, trees_oblique_ir (MWIR). Pins: the sim clock frozen (CAMSIM_SIM_TIME_RATE=0, and the host sends
the same date and time every frame) and the sensor's temporal noise keyed to one frame
(`-ExecCmds=camsim.Sensor.FixedNoiseFrame 0`). Each pose's CIGI datagram is a pure function of the pose but for the
IG Control's frame counter and wall-clock timestamp; its SHA-256 with those zeroed is in the report, so both runs provably got the same input.
Per pose, after /ready terrain: a /snapshot every second until three in a row have the same pixel hash (SHA-256 of
the decoded RGB), up to 90 s; that settled frame is kept (OUT/<run>/<pose>.png).
Gates: S1 every pose settles within 90 s in both runs; S2 run a's settled hash equals run b's for every pose. On a
mismatch: differing pixels, max |diff|, their bounding box and OUT/diff/<pose>.png. --against OTHER_OUT compares
this machine's settled frames with another's (SSIM on luma, mean |diff|; informational: no cross-GPU threshold yet).
Exit 0 when S1 and S2 pass, 1 on a gate failure, 2 on an error."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "scripts" / "bench"))
from bench import scenario  # noqa: E402

SETTLE_MAX_S = 90.0
SETTLE_NEED = 3
EXEC_CMDS = "-ExecCmds=camsim.Sensor.FixedNoiseFrame 0"


@dataclass(frozen=True)
class NamedPose:
    name: str
    pose: scenario.Pose


_CAMP = scenario.Pose(33.3010, -117.3480, 300.0, yaw=30.0, gimbal_pitch=-90.0, fov_h=30.0)
_TREES = scenario.Pose(33.2980, -117.3540, 300.0, yaw=0.0, gimbal_pitch=-20.0, fov_h=30.0)
POSES = [
    NamedPose("camp_nadir", _CAMP),  # MCAS Camp Pendleton airfield buildings (R3 tileset)
    NamedPose("trees_oblique", _TREES),  # Santa Margarita riparian woodland from 1 km south (R4 near cells)
    NamedPose("slant_5km", scenario.Pose(33.2750, -117.3920, 650.0, yaw=45.0, gimbal_pitch=-12.0, fov_h=30.0)),
    NamedPose("coast_waves", scenario.Pose(33.2100, -117.4000, 150.0, yaw=270.0, gimbal_pitch=-15.0, fov_h=40.0)),
    NamedPose("camp_nadir_ir", scenario.Pose(**{**_CAMP.__dict__, "sensor_id": 1})),
    NamedPose("trees_oblique_ir", scenario.Pose(**{**_TREES.__dict__, "sensor_id": 1})),
]


def pixel_hash(img: np.ndarray) -> str:
    return hashlib.sha256(np.ascontiguousarray(img).tobytes() + str(img.shape).encode()).hexdigest()


def datagram_digest(pose: scenario.Pose) -> str:
    """SHA-256 of the pose's host datagram with the IG Control's frame counter (bytes 8-11) and wall-clock timestamp
    (bytes 12-15) zeroed: everything else is a function of the pose."""
    d = bytearray(scenario.host_datagram(0, pose))
    d[8:16] = bytes(8)
    return hashlib.sha256(bytes(d)).hexdigest()


def settle(fetch: Callable[[], np.ndarray | None], max_s: float = SETTLE_MAX_S, need: int = SETTLE_NEED,
           now: Callable[[], float] = time.monotonic, sleep: Callable[[float], None] = time.sleep) -> dict:
    """Fetch a frame a second until `need` in a row have the same pixel hash, or max_s passes."""
    t0, run, last, n, img = now(), 0, None, 0, None
    while True:
        frame = fetch()
        if frame is not None:
            n += 1
            h = pixel_hash(frame)
            run = run + 1 if h == last else 1
            last, img = h, frame
            if run >= need:
                return {"settled": True, "hash": h, "image": img, "frames": n, "seconds": now() - t0}
        if now() - t0 >= max_s:
            return {"settled": False, "hash": last, "image": img, "frames": n, "seconds": now() - t0}
        sleep(1.0)


def diff_stats(a: np.ndarray, b: np.ndarray) -> dict:
    d = np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2)
    ys, xs = np.nonzero(d)
    if len(ys) == 0:
        return {"equal": True, "pixels": 0, "max": 0, "bbox": None}
    return {"equal": False, "pixels": int(len(ys)), "max": int(d.max()),
            "bbox": [int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1]}


def ssim(a: np.ndarray, b: np.ndarray, block: int = 8) -> float:
    """Mean SSIM of the luma over non-overlapping block x block windows (C1, C2 for 8-bit)."""
    def luma(x):
        x = x.astype(np.float64)
        return 0.299 * x[..., 0] + 0.587 * x[..., 1] + 0.114 * x[..., 2]

    ya, yb = luma(a), luma(b)
    h, w = (ya.shape[0] // block) * block, (ya.shape[1] // block) * block
    ya = ya[:h, :w].reshape(h // block, block, w // block, block)
    yb = yb[:h, :w].reshape(h // block, block, w // block, block)
    ma, mb = ya.mean(axis=(1, 3)), yb.mean(axis=(1, 3))
    va, vb = ya.var(axis=(1, 3)), yb.var(axis=(1, 3))
    cov = ((ya - ma[:, None, :, None]) * (yb - mb[:, None, :, None])).mean(axis=(1, 3))
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    s = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma**2 + mb**2 + c1) * (va + vb + c2))
    return float(s.mean())


def _png(img: np.ndarray, path: Path) -> None:
    from PIL import Image

    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(img).save(path)


def _load(path: Path) -> np.ndarray:
    from PIL import Image

    return np.asarray(Image.open(path).convert("RGB"))


def run_once(label: str, pkg: Path, out: Path) -> dict:
    sys.path.insert(0, str(REPO / "scripts" / "scene" / "tools"))
    import camsim_session as cs
    import urllib.request

    rb = cs.rb

    def fetch() -> np.ndarray | None:
        try:
            with urllib.request.urlopen(f"http://{rb.HEALTH}/snapshot", timeout=10) as r:
                if r.status != 200:
                    return None
                from PIL import Image

                return np.asarray(Image.open(io.BytesIO(r.read())).convert("RGB"))
        except OSError:
            return None

    env = {**cs.env_for("package", pkg), "CAMSIM_SIM_TIME_RATE": "0", "CAMSIM_DIS_ENABLED": "0",
           "CAMSIM_MULTICAST_ADDR": "127.0.0.1"}
    res = {}
    with cs.camsim(env, POSES[0].pose, extra_args=[EXEC_CMDS]) as host:
        for p in POSES:
            print(f"[determinism] {label} {p.name}", flush=True)
            host.pose = p.pose
            time.sleep(1.0)
            rb.wait_terrain(180)
            r = settle(fetch)
            if r["image"] is not None:
                _png(r["image"], out / label / f"{p.name}.png")
            res[p.name] = {k: v for k, v in r.items() if k != "image"}
    return res


def evaluate(out: Path, runs: dict) -> dict:
    report: dict = {"poses": {}, "datagrams": {p.name: datagram_digest(p.pose) for p in POSES}}
    s1_failed, s2_failed = [], []
    for p in POSES:
        ra, rb_ = runs["a"].get(p.name, {}), runs["b"].get(p.name, {})
        row = {"a": ra, "b": rb_}
        if not (ra.get("settled") and rb_.get("settled")):
            s1_failed.append(p.name)
        fa, fb = out / "a" / f"{p.name}.png", out / "b" / f"{p.name}.png"
        if fa.exists() and fb.exists():
            ia, ib = _load(fa), _load(fb)
            d = diff_stats(ia, ib) if ia.shape == ib.shape else {"equal": False, "shape": [ia.shape, ib.shape]}
            row["diff"] = d
            if not d["equal"]:
                s2_failed.append(p.name)
                if ia.shape == ib.shape:
                    dm = np.abs(ia.astype(np.int16) - ib.astype(np.int16)).max(axis=2)
                    _png(np.clip(dm * 16, 0, 255).astype(np.uint8)[..., None].repeat(3, axis=2),
                         out / "diff" / f"{p.name}.png")
        else:
            s2_failed.append(p.name)
        report["poses"][p.name] = row
    report["gates"] = {"S1": {"pass": not s1_failed, "failed": s1_failed},
                       "S2": {"pass": not s2_failed, "failed": s2_failed}}
    return report


def against(out: Path, other: Path) -> dict:
    res = {}
    for p in POSES:
        fa, fb = out / "a" / f"{p.name}.png", other / "a" / f"{p.name}.png"
        if fa.exists() and fb.exists():
            ia, ib = _load(fa), _load(fb)
            if ia.shape == ib.shape:
                res[p.name] = {"ssim": ssim(ia, ib),
                               "mean_abs_diff": float(np.abs(ia.astype(np.int16) - ib.astype(np.int16)).mean())}
    return res


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scene", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--against", type=Path)
    ap.add_argument("--check-only", action="store_true")
    a = ap.parse_args()
    runs_file = a.out / "runs.json"
    if a.check_only:
        runs = json.loads(runs_file.read_text())
    else:
        try:
            runs = {label: run_once(label, a.scene.resolve(), a.out) for label in ("a", "b")}
        except RuntimeError as e:
            print(f"error: {e}", file=sys.stderr)
            return 2
        a.out.mkdir(parents=True, exist_ok=True)
        runs_file.write_text(json.dumps(runs, indent=1) + "\n")
    report = evaluate(a.out, runs)
    report.update(scene=a.scene.name, date=time.strftime("%Y-%m-%d"))
    if a.against:
        report["against"] = {"other": str(a.against), "poses": against(a.out, a.against)}
    (a.out / "report.json").write_text(json.dumps(report, indent=1) + "\n")
    print("pose | settled a / b (s, frames) | equal | differing px | max")
    for name, row in report["poses"].items():
        sa, sb, d = row["a"], row["b"], row.get("diff", {})
        print(f"  {name:<17} {sa.get('settled')} {sa.get('seconds', 0):.0f}s/{sa.get('frames')} | "
              f"{sb.get('settled')} {sb.get('seconds', 0):.0f}s/{sb.get('frames')} | {d.get('equal')} | "
              f"{d.get('pixels')} | {d.get('max')}")
    for k, v in report["gates"].items():
        print(f"{k}: {'PASS' if v['pass'] else 'FAIL ' + ', '.join(v['failed'])}")
    return 0 if all(v["pass"] for v in report["gates"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
