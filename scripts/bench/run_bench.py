"""Run the render benchmark against a local CamSim (ROADMAP 3A).

Launches CamSim headless via scripts/run.sh with frame stats and /snapshot on,
flies scenario.py over CIGI, saves reference shots and the slew-phase stream,
then writes per-phase metrics.

Usage:
  uv run --with numpy --with pillow python scripts/bench/run_bench.py --label baseline
  ... --view-source scene_capture | --smoke | --trace | --skip-warmup | --out DIR
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from bench import analyze, scenario  # noqa: E402

HEALTH = f"http://127.0.0.1:{os.environ.get('CAMSIM_HEALTH_HTTP_PORT', '8080')}"
CIGI_PORT = int(os.environ.get("CAMSIM_CIGI_PORT", "8888"))
STREAM_PORT = int(os.environ.get("CAMSIM_MULTICAST_PORT", "5004"))
READY_TIMEOUT_S = 900       # a cold start compiles shaders
PID_FILE_GRACE_S = 30.0     # run.sh --detach writes it before returning
SHOT_SETTLE_S = 2.0         # TSR / Lumen history after the terrain gate opens


class Host:
    """Sends one CIGI host frame at 30 Hz for whatever pose is current."""

    def __init__(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.pose = scenario.build_phases()[1].pose_at(0.0)
        self.frame = 0
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        period = 1.0 / 30.0
        next_t = time.monotonic()
        while not self.stop.is_set():
            self.sock.sendto(scenario.host_datagram(self.frame, self.pose), ("127.0.0.1", CIGI_PORT))
            self.frame += 1
            next_t += period
            time.sleep(max(0.0, next_t - time.monotonic()))


def http_json(path: str) -> dict | None:
    try:
        with urllib.request.urlopen(HEALTH + path, timeout=3) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read())
        except Exception:
            return None
    except Exception:
        return None


def http_text(path: str) -> str | None:
    try:
        with urllib.request.urlopen(HEALTH + path, timeout=3) as r:
            return r.read().decode("utf-8")
    except Exception:
        return None


def camsim_alive(pid_file: Path) -> bool | None:
    """True/False from the pid in run.sh's pid file; None if there is no pid file."""
    if not pid_file.exists():
        return None
    pid = pid_file.read_text().split()[0]
    return subprocess.run(["kill", "-0", pid], capture_output=True).returncode == 0


def wait_ready(pid_file: Path, pid_grace_s: float = PID_FILE_GRACE_S) -> None:
    start = time.time()
    deadline = start + READY_TIMEOUT_S
    while time.time() < deadline:
        alive = camsim_alive(pid_file)
        if alive is False:
            sys.exit("CamSim exited during startup")
        if alive is None and time.time() - start >= pid_grace_s:
            sys.exit(f"run.sh --detach wrote no pid file ({pid_file}) within {pid_grace_s:.0f}s")
        body = http_json("/ready")
        if body and body.get("status") == "ready":
            return
        time.sleep(2)
    sys.exit(f"/ready not reached within {READY_TIMEOUT_S}s")


def wait_terrain(timeout_s: float = 60.0) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        body = http_json("/ready")
        if body and body.get("terrain_ready"):
            return True
        time.sleep(0.5)
    return False


def fetch_snapshot(dest: Path, route: str = "/snapshot") -> bool:
    try:
        with urllib.request.urlopen(HEALTH + route, timeout=10) as r:
            dest.write_bytes(r.read())
            return True
    except Exception as e:  # noqa: BLE001 - report and carry on to the next shot
        print(f"[bench] snapshot {dest.name} failed: {e}")
        return False


def sensor_path_from_metrics(text: str) -> str | None:
    for line in text.splitlines():
        if line.startswith("camsim_sensor_path{"):
            return line.split('path="', 1)[1].split('"', 1)[0]
    return None


def port_busy(netstat: str, port: int) -> bool:
    """True if a `netstat -an` socket has `port` as its LOCAL address (4th
    column): macOS writes 127.0.0.1.8080, Linux 127.0.0.1:8080. An outbound
    connection to a remote port 8080 doesn't count."""
    for line in netstat.splitlines():
        cols = line.split()
        if len(cols) >= 4 and cols[0].startswith("tcp") and cols[3].endswith((f".{port}", f":{port}")):
            return True
    return False


def wait_port_free(port: int, timeout_s: float = 90.0) -> None:
    """Wait out TIME_WAIT on the health port so timings start clean. (CamSim
    would retry the bind itself, but /ready would come up late.)"""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        netstat = subprocess.run(["netstat", "-an", "-p", "tcp"], capture_output=True, text=True).stdout
        if not port_busy(netstat, port):
            return
        time.sleep(2)
    print(f"[bench] warning: port {port} still busy after {timeout_s:.0f}s")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--view-source", choices=["primary", "scene_capture"], default=None)
    ap.add_argument("--sensor-path", choices=["auto", "gpu", "legacy"], default=None)
    ap.add_argument("--smoke", action="store_true")
    ap.add_argument("--trace", action="store_true", help="also record an Unreal Insights trace")
    ap.add_argument("--skip-warmup", action="store_true")
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()

    # Absolute: CamSim resolves relative paths against its own working directory.
    out = (args.out or REPO / ".cache" / "bench" / f"{time.strftime('%Y%m%d-%H%M%S')}-{args.label}").resolve()
    (out / "shots").mkdir(parents=True, exist_ok=True)
    pid_file = REPO / ".cache" / "camsim.pid"
    if camsim_alive(pid_file):
        sys.exit("A CamSim instance is already running (scripts/stop.sh to stop it)")

    env = dict(os.environ,
               CAMSIM_FRAME_STATS_PATH=str(out / "frames.jsonl"),
               CAMSIM_SNAPSHOT_ENDPOINT_ENABLED="1")
    if args.view_source:
        env["CAMSIM_RENDER_VIEW_SOURCE"] = args.view_source
    if args.sensor_path:
        env["CAMSIM_RENDER_SENSOR_PATH"] = args.sensor_path
    extra = ["-trace=cpu,gpu,frame", f"-tracefile={out / 'trace.utrace'}"] if args.trace else []

    wait_port_free(int(HEALTH.rsplit(":", 1)[1]))
    host = Host()
    host.thread.start()   # /ready needs CIGI traffic
    subprocess.run([str(REPO / "scripts" / "run.sh"), "--headless", "--local", "--detach", *extra],
                   env=env, check=True, stdout=subprocess.DEVNULL)
    phases_log: list[dict] = []
    sensor_path: str | None = None
    try:
        wait_ready(pid_file)
        metrics = http_text("/metrics") or ""
        sensor_path = sensor_path_from_metrics(metrics)
        print(f"[bench] sensor path: {sensor_path}", flush=True)
        if args.sensor_path in ("gpu", "legacy") and sensor_path != args.sensor_path:
            sys.exit(f"[bench] expected sensor path {args.sensor_path}, CamSim reports {sensor_path}")
        for ph in scenario.build_phases(smoke=args.smoke):
            if ph.name == "warmup" and args.skip_warmup:
                continue
            print(f"[bench] phase {ph.name} ({ph.duration_s:.0f}s)", flush=True)
            host.pose = ph.pose_at(0.0)
            if ph.measured:
                wait_terrain()          # start measuring from a loaded view
            ts = None
            if ph.name == "slew":
                ts = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error",
                                       "-i", f"udp://127.0.0.1:{STREAM_PORT}?timeout=5000000",
                                       "-t", str(ph.duration_s), "-c", "copy", str(out / "slew.ts")])
            start = time.time()
            while (t := time.time() - start) < ph.duration_s:
                host.pose = ph.pose_at(t)
                time.sleep(1.0 / 60.0)
            phases_log.append({"name": ph.name, "start": start, "end": time.time(), "measured": ph.measured})
            if ts:
                try:
                    ts.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    ts.kill()

        for shot in scenario.build_shots(smoke=args.smoke):
            host.pose = shot.pose
            time.sleep(1.0)             # let the pose arrive before checking the gate
            if not wait_terrain():
                print(f"[bench] shot {shot.name}: terrain gate never opened")
            time.sleep(SHOT_SETTLE_S)
            fetch_snapshot(out / "shots" / f"{shot.name}.png")
            fetch_snapshot(out / "shots" / f"{shot.name}_sensor.png", route="/snapshot/sensor")
    finally:
        host.stop.set()
        subprocess.run([str(REPO / "scripts" / "stop.sh")], check=False, stdout=subprocess.DEVNULL)

    (out / "phases.json").write_text(json.dumps(phases_log, indent=2))
    rows = analyze.load_rows(out / "frames.jsonl")
    sha = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True, cwd=REPO).stdout.strip()
    results = {
        "meta": {"label": args.label, "git": sha, "platform": platform.platform(),
                 "machine": platform.machine(), "view_source": args.view_source or "config default",
                 "sensor_path": sensor_path,
                 "warmup_ran": not args.skip_warmup, "smoke": args.smoke},
        "phases": analyze.summarize(rows, phases_log),
    }
    (out / "results.json").write_text(json.dumps(results, indent=2))
    print(json.dumps(results["phases"], indent=2))
    print(f"[bench] results in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
