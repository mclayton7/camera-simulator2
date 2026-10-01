"""Gimbal-snap test: how long the view stays coarse after an instant 90-degree slew.

Run against a CamSim already started with frame stats and /snapshot on (e.g. the
Docker image with CAMSIM_FRAME_STATS_PATH and CAMSIM_SNAPSHOT_ENDPOINT_ENABLED=1).
Holds a pose over San Francisco until the tilesets settle, then snaps the gimbal
yaw by 90 degrees in one frame (twice, to both sides). For each snap it reports
the time until Cesium's load progress returns to 100% and saves snapshots at
+0.3 s, +1 s and +3 s. Compare settings (frustum_culling, maximum_screen_space_error)
by running it once per configuration.

Usage: python3 scripts/bench/snap_test.py FRAMES_JSONL OUT_DIR
"""

from __future__ import annotations

import json
import socket
import sys
import threading
import time
import urllib.request
from dataclasses import replace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bench import scenario  # noqa: E402

HEALTH = "http://127.0.0.1:8080"
SETTLE_S = 25.0
SHOTS_S = (0.3, 1.0, 3.0)
WATCH_S = 8.0


def snapshot(dest: Path) -> None:
    with urllib.request.urlopen(HEALTH + "/snapshot", timeout=10) as r:
        dest.write_bytes(r.read())


def frames_since(path: Path, t0: float) -> list[dict]:
    out = []
    with open(path) as f:
        for line in f:
            if line.strip():
                r = json.loads(line)
                if r["t"] >= t0:
                    out.append(r)
    return out


def main() -> int:
    frames_path, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    pose = scenario.Pose(scenario.BASE_LAT, scenario.BASE_LON, 3000.0, gimbal_yaw=0.0, gimbal_pitch=-35.0)
    state = {"pose": pose, "stop": False}
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def host() -> None:
        frame = 0
        while not state["stop"]:
            sock.sendto(scenario.host_datagram(frame, state["pose"]), ("127.0.0.1", 8888))
            frame += 1
            time.sleep(1 / 30)

    threading.Thread(target=host, daemon=True).start()
    deadline = time.time() + 600
    while time.time() < deadline:  # /ready needs the CIGI traffic above
        try:
            with urllib.request.urlopen(HEALTH + "/ready", timeout=2) as r:
                if json.loads(r.read()).get("status") == "ready":
                    break
        except Exception:  # noqa: BLE001 -- polling
            pass
        time.sleep(1)

    results = []
    for i, yaw in enumerate((90.0, -90.0)):
        time.sleep(SETTLE_S)
        settle = frames_since(frames_path, time.time() - 5.0)
        t_snap = time.time()
        state["pose"] = replace(pose, gimbal_yaw=yaw)
        for s in SHOTS_S:
            time.sleep(max(0.0, t_snap + s - time.time()))
            snapshot(out / f"snap{i}_{yaw:+.0f}_{s:.1f}s.png")
        time.sleep(max(0.0, t_snap + WATCH_S - time.time()))
        after = frames_since(frames_path, t_snap)
        loading = [r for r in after if r["load_pct"] < 100.0]
        done = next((r["t"] - t_snap for r in after if r["t"] - t_snap > 0.1 and r["load_pct"] >= 100.0
                     and all(x["load_pct"] >= 100.0 for x in after if x["t"] >= r["t"])), None)
        g = sorted(r["game_ms"] for r in settle)
        results.append({
            "snap": yaw,
            "load_complete_s": round(done, 2) if done is not None else None,
            "frames_loading": len(loading),
            "max_wall_ms_after": round(max(r["wall_ms"] for r in after), 1),
            "settled_game_ms_p50": round(g[len(g) // 2], 2),
        })
        print(json.dumps(results[-1]), flush=True)
        state["pose"] = pose
    state["stop"] = True
    (out / "snap_results.json").write_text(json.dumps(results, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
