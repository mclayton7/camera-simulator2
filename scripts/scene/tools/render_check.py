"""R0 gate 5: launch CamSim on a scene package (CAMSIM_SCENE_DIR) or on Cesium ion ("cwt"), fly fixed shots
over Camp Pendleton and save /snapshot PNGs + result.json.

    uv run --project scripts/scene python scripts/scene/tools/render_check.py OUT package PKG [--offline]
    uv run --project scripts/scene python scripts/scene/tools/render_check.py OUT cwt

--offline (macOS) runs CamSim under sandbox-exec with ports 80/443 blocked (tools/offline.sb) and
CAMSIM_SCENE_OFFLINE=1, and fails if the log shows any outbound request.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

from camsim_session import Pose as P
from camsim_session import camsim, env_for, rb

LOG = Path.home() / "Library" / "Logs" / "CamSimTest" / "CamSimTest.log"  # macOS editor log (run.sh)


def network_lines(log: Path) -> list[str]:
    """Log lines that show an outbound request: UE's HTTP client (category LogHttp, not CamSim's health server's
    LogHttpServerModule/LogHttpListener) or anything naming cesium.com. (UE's local Zen service logs an
    http://[::1] URL; that is not a request.) Tokens are cut off."""
    if not log.exists():
        return []
    hits = [ln for ln in log.read_text(errors="replace").splitlines() if "LogHttp:" in ln or "cesium.com" in ln]
    return [ln.split("access_token=")[0] for ln in hits]


SHOTS = {
    "nadir_2km": P(lat=33.225, lon=-117.380, alt=2000, gimbal_pitch=-90, fov_h=30),
    "slant_ne": P(lat=33.205, lon=-117.405, alt=800, yaw=45, gimbal_pitch=-20, fov_h=40),
    "rivermouth": P(lat=33.222, lon=-117.400, alt=250, yaw=90, gimbal_pitch=-25, fov_h=50),
    "interior_nadir": P(lat=33.380, lon=-117.420, alt=2000, gimbal_pitch=-90, fov_h=30),  # 1/3" terrain only
    "high_oblique": P(lat=33.300, lon=-117.450, alt=6000, yaw=30, gimbal_pitch=-10, fov_h=60),
    "ring_edge": P(lat=33.350, lon=-117.430, alt=9000, yaw=270, gimbal_pitch=-12, fov_h=60),  # over the ring
    "bbox_edge": P(lat=33.430, lon=-117.560, alt=3000, yaw=270, gimbal_pitch=-15, fov_h=60),  # across the NAIP edge
    "sea_offshore": P(lat=33.215, lon=-117.410, alt=600, yaw=240, gimbal_pitch=-30, fov_h=50),  # shore -> open sea
    "slant_ne_ir": P(lat=33.205, lon=-117.405, alt=800, yaw=45, gimbal_pitch=-20, fov_h=40, sensor_id=1),
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", type=Path)
    ap.add_argument("mode", choices=["package", "cwt"])
    ap.add_argument("pkg", type=Path, nargs="?")
    ap.add_argument("--offline", action="store_true")
    a = ap.parse_args()
    if a.mode == "package" and not a.pkg:
        ap.error("package mode needs PKG")
    a.out.mkdir(parents=True, exist_ok=True)
    res: dict = {"mode": a.mode, "offline": a.offline}
    t0 = time.time()
    with camsim(env_for(a.mode, a.pkg and a.pkg.resolve()), SHOTS["nadir_2km"], a.offline) as host:
        res["ready_s"] = round(time.time() - t0)
        for name, pose in SHOTS.items():
            host.pose = pose
            time.sleep(1.0)
            ok = rb.wait_terrain(90)
            time.sleep(rb.SHOT_SETTLE_S + 2)
            rb.fetch_snapshot(a.out / f"{name}.png")
            res[name] = {"terrain_ready": ok}
            print(name, ok, flush=True)
        metrics = (rb.http_text("/metrics") or "").splitlines()
        res["metrics"] = [m for m in metrics if "tile" in m.lower() or "terrain" in m.lower()][:20]
    res["network_lines"] = network_lines(LOG)[:20]
    (a.out / "result.json").write_text(json.dumps(res, indent=1))
    if a.offline and res["network_lines"]:
        print("FAIL: offline run logged network use:", *res["network_lines"], sep="\n  ")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
