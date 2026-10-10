"""Run the render benchmark against a local CamSim (ROADMAP 3A).

Launches CamSim headless via scripts/run.sh (or a Docker image, --docker)
with frame stats and /snapshot on, flies scenario.py over CIGI, saves
reference shots and the slew-phase stream, then writes per-phase metrics.

Usage:
  uv run --with numpy --with pillow python scripts/bench/run_bench.py --label baseline
  ... --smoke | --trace | --skip-warmup | --out DIR
  ... --docker camsim:latest [--no-gpu] [--env CAMSIM_ENCODER=libx264]
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from dataclasses import replace
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from bench import analyze, scenario

HEALTH = f"http://127.0.0.1:{os.environ.get('CAMSIM_HEALTH_HTTP_PORT', '8080')}"
CIGI_PORT = int(os.environ.get("CAMSIM_CIGI_PORT", "8888"))
STREAM_PORT = int(os.environ.get("CAMSIM_MULTICAST_PORT", "5004"))
READY_TIMEOUT_S = 900  # a cold start compiles shaders
PID_FILE_GRACE_S = 30.0  # run.sh --detach writes it before returning
SHOT_SETTLE_S = 2.0  # TSR / Lumen history after the terrain gate opens
DOCKER_NAME = "camsim-bench"
# Named volumes keep the Cesium tile cache and the driver shader cache warm
# across --docker runs, as the native DDC/Cesium caches are between run.sh runs.
DOCKER_VOLUMES = {
    "camsim-bench-data": "/var/lib/camsim",
    "camsim-bench-cache": "/home/camsim/.cache",
}


class Host:
    """Sends one CIGI host frame at 30 Hz for whatever pose is current."""

    def __init__(self, site: scenario.Site = scenario.SITES["sf"]) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.pose = scenario.build_phases(site=site)[1].pose_at(0.0)
        self.frame = 0
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        period = 1.0 / 30.0
        next_t = time.monotonic()
        while not self.stop.is_set():
            self.sock.sendto(
                scenario.host_datagram(self.frame, self.pose), ("127.0.0.1", CIGI_PORT)
            )
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
        except Exception:  # noqa: BLE001 -- polling helper: any failure reads as "no answer"
            return None
    except Exception:  # noqa: BLE001 -- polling helper: any failure reads as "no answer"
        return None


def http_text(path: str) -> str | None:
    try:
        with urllib.request.urlopen(HEALTH + path, timeout=3) as r:
            return r.read().decode("utf-8")
    except Exception:  # noqa: BLE001 -- polling helper: any failure reads as "no answer"
        return None


def camsim_alive(pid_file: Path) -> bool | None:
    """True/False from the pid in run.sh's pid file; None if there is no pid file."""
    if not pid_file.exists():
        return None
    pid = pid_file.read_text().split()[0]
    return (
        subprocess.run(["kill", "-0", pid], capture_output=True, check=False).returncode
        == 0
    )


def docker_alive(name: str = DOCKER_NAME) -> bool | None:
    """True/False from the container's state; None if there is no such container."""
    r = subprocess.run(
        ["docker", "inspect", "-f", "{{.State.Running}}", name],
        capture_output=True,
        text=True,
        check=False,
    )
    if r.returncode != 0:
        return None
    return r.stdout.strip() == "true"


def docker_run_cmd(
    image: str,
    out: Path,
    gpu: bool,
    env: dict[str, str],
    ue_args: list[str],
    config: Path | None = None,
) -> list[str]:
    """`docker run` for a bench container: host network (CIGI + unicast stream on
    loopback), the output dir at /bench, cache volumes, the GPU unless --no-gpu."""
    cmd = ["docker", "run", "-d", "--name", DOCKER_NAME, "--init"]
    cmd += ["--network", "host", "--shm-size", "1g", "-v", f"{out}:/bench"]
    for vol, mount in DOCKER_VOLUMES.items():
        cmd += ["-v", f"{vol}:{mount}"]
    if config:
        cmd += [
            "-v",
            f"{config.resolve()}:/opt/camsim/CamSimTest/camsim_config.yaml:ro",
        ]
    if gpu:
        cmd += ["--gpus", "all"]
    for key, value in env.items():
        cmd += ["-e", f"{key}={value}"]
    return [*cmd, image, *ue_args]


def encoder_from_log(text: str) -> str | None:
    m = re.search(r"FVideoEncoder: using encoder (\S+)", text)
    return m.group(1) if m else None


def latency_from_metrics(text: str) -> dict:
    """Latency summaries on /metrics (CAMSIM_TRACK_PIPELINE_LATENCY=1):
    camsim_frame_latency_ms{quantile="0.5"} 13.9 -> {"frame_latency_ms_p50": 13.9}."""
    out = {}
    for m in re.finditer(
        r'^camsim_(\w*latency\w*)\{quantile="([0-9.]+)"\} ([0-9.eE+-]+)$',
        text,
        re.MULTILINE,
    ):
        out[f"{m.group(1)}_p{round(float(m.group(2)) * 100)}"] = float(m.group(3))
    return out


def wait_ready(
    pid_file: Path | None,
    pid_grace_s: float = PID_FILE_GRACE_S,
    alive_fn=None,
) -> None:
    """Wait for /ready. Liveness comes from run.sh's pid file, or from
    `alive_fn` (True/False/None, None = not started yet) when given."""
    start = time.time()
    deadline = start + READY_TIMEOUT_S
    while time.time() < deadline:
        alive = alive_fn() if alive_fn else camsim_alive(pid_file)
        if alive is False:
            sys.exit("CamSim exited during startup")
        if alive is None and time.time() - start >= pid_grace_s:
            sys.exit(
                f"run.sh --detach wrote no pid file ({pid_file}) within {pid_grace_s:.0f}s"
                if not alive_fn
                else f"CamSim did not start within {pid_grace_s:.0f}s"
            )
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
        if (
            len(cols) >= 4
            and cols[0].startswith("tcp")
            and cols[3].endswith((f".{port}", f":{port}"))
        ):
            return True
    return False


def wait_port_free(port: int, timeout_s: float = 90.0) -> None:
    """Wait out TIME_WAIT on the health port so timings start clean. (CamSim
    would retry the bind itself, but /ready would come up late.)"""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        netstat = subprocess.run(
            ["netstat", "-an", "-p", "tcp"], capture_output=True, text=True, check=False
        ).stdout
        if not port_busy(netstat, port):
            return
        time.sleep(2)
    print(f"[bench] warning: port {port} still busy after {timeout_s:.0f}s")


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--smoke", action="store_true")
    ap.add_argument(
        "--trace", action="store_true", help="also record an Unreal Insights trace"
    )
    ap.add_argument(
        "--site",
        choices=sorted(scenario.SITES),
        default="sf",
        help="base of the flight: sf (default) or pendleton (adds a coastal low pass)",
    )
    ap.add_argument("--skip-warmup", action="store_true")
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument(
        "--docker",
        metavar="IMAGE",
        default=None,
        help="run this CamSim image instead of the native build (docs/docker.md)",
    )
    ap.add_argument(
        "--no-gpu",
        action="store_true",
        help="--docker without --gpus all: Mesa lavapipe (CPU Vulkan) + libx264",
    )
    ap.add_argument(
        "--env",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="extra CamSim environment, e.g. CAMSIM_ENCODER=libx264 (repeatable)",
    )
    ap.add_argument(
        "--sensor",
        choices=["eo", "ir"],
        default="eo",
        help="waveband for the measured phases (shots keep their own)",
    )
    ap.add_argument(
        "--config",
        type=Path,
        default=None,
        metavar="FILE",
        help="--docker: mount this camsim_config.yaml over the image's (e.g. 1080p)",
    )
    return ap


def with_sensor(pose: scenario.Pose, sensor: str) -> scenario.Pose:
    """The phase pose in the requested waveband (0 EO, 1 IR)."""
    return replace(pose, sensor_id=1 if sensor == "ir" else 0)


def main() -> int:
    args = build_parser().parse_args()
    site = scenario.SITES[args.site]

    # Absolute: CamSim resolves relative paths against its own working directory.
    out = (
        args.out
        or REPO / ".cache" / "bench" / f"{time.strftime('%Y%m%d-%H%M%S')}-{args.label}"
    ).resolve()
    (out / "shots").mkdir(parents=True, exist_ok=True)
    pid_file = REPO / ".cache" / "camsim.pid"
    if camsim_alive(pid_file):
        sys.exit("A CamSim instance is already running (scripts/stop.sh to stop it)")
    if (args.no_gpu or args.config) and not args.docker:
        sys.exit("--no-gpu and --config need --docker")
    extra_env = dict(kv.split("=", 1) for kv in args.env)

    # Paths as CamSim sees them: the container mounts `out` at /bench.
    cs_out = Path("/bench") if args.docker else out
    camsim_env = {
        "CAMSIM_FRAME_STATS_PATH": str(cs_out / "frames.jsonl"),
        "CAMSIM_SNAPSHOT_ENDPOINT_ENABLED": "1",
        **extra_env,
    }
    extra = (
        ["-trace=cpu,gpu,frame", f"-tracefile={cs_out / 'trace.utrace'}"]
        if args.trace
        else []
    )

    wait_port_free(int(HEALTH.rsplit(":", 1)[1]))
    host = Host(site)
    host.thread.start()  # /ready needs CIGI traffic
    if args.docker:
        subprocess.run(
            ["docker", "rm", "-f", DOCKER_NAME], capture_output=True, check=False
        )
        out.chmod(0o777)  # the container writes as uid 1000
        (out / "shots").chmod(0o777)
        camsim_env.setdefault("CAMSIM_MULTICAST_ADDR", "127.0.0.1")  # as run.sh --local
        if os.environ.get("CAMSIM_CESIUM_ION_TOKEN"):
            camsim_env["CAMSIM_CESIUM_ION_TOKEN"] = os.environ[
                "CAMSIM_CESIUM_ION_TOKEN"
            ]
        subprocess.run(
            docker_run_cmd(
                args.docker, out, not args.no_gpu, camsim_env, extra, args.config
            ),
            check=True,
            stdout=subprocess.DEVNULL,
        )
    else:
        subprocess.run(
            [
                str(REPO / "scripts" / "run.sh"),
                "--headless",
                "--local",
                "--detach",
                *extra,
            ],
            env=dict(os.environ, **camsim_env),
            check=True,
            stdout=subprocess.DEVNULL,
        )
    phases_log: list[dict] = []
    sensor_path: str | None = None
    latency: dict = {}
    ready_s: float | None = None
    t_launch = time.time()
    try:
        if args.docker:
            wait_ready(None, alive_fn=docker_alive)
        else:
            wait_ready(pid_file)
        ready_s = time.time() - t_launch
        metrics = http_text("/metrics") or ""
        sensor_path = sensor_path_from_metrics(metrics)
        print(
            f"[bench] ready after {ready_s:.0f}s, sensor path: {sensor_path}",
            flush=True,
        )
        for ph in scenario.build_phases(smoke=args.smoke, site=site):
            if ph.name == "warmup" and args.skip_warmup:
                continue
            print(f"[bench] phase {ph.name} ({ph.duration_s:.0f}s)", flush=True)
            host.pose = with_sensor(ph.pose_at(0.0), args.sensor)
            if ph.measured:
                wait_terrain()  # start measuring from a loaded view
            ts = None
            if ph.name == "slew":
                ts = subprocess.Popen(
                    [
                        "ffmpeg",
                        "-y",
                        "-loglevel",
                        "error",
                        "-i",
                        f"udp://127.0.0.1:{STREAM_PORT}?timeout=5000000",
                        "-t",
                        str(ph.duration_s),
                        "-c",
                        "copy",
                        str(out / "slew.ts"),
                    ]
                )
            start = time.time()
            while (t := time.time() - start) < ph.duration_s:
                host.pose = with_sensor(ph.pose_at(t), args.sensor)
                time.sleep(1.0 / 60.0)
            phases_log.append(
                {
                    "name": ph.name,
                    "start": start,
                    "end": time.time(),
                    "measured": ph.measured,
                }
            )
            if ts:
                try:
                    ts.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    ts.kill()

        metrics_end = http_text("/metrics") or ""
        (out / "metrics.txt").write_text(metrics_end)
        latency = latency_from_metrics(metrics_end)

        for shot in scenario.build_shots(smoke=args.smoke, site=site):
            host.pose = shot.pose
            time.sleep(1.0)  # let the pose arrive before checking the gate
            if not wait_terrain():
                print(f"[bench] shot {shot.name}: terrain gate never opened")
            time.sleep(SHOT_SETTLE_S)
            fetch_snapshot(out / "shots" / f"{shot.name}.png")
            fetch_snapshot(
                out / "shots" / f"{shot.name}_sensor.png", route="/snapshot/sensor"
            )
    finally:
        host.stop.set()
        if args.docker:
            log = subprocess.run(
                ["docker", "logs", DOCKER_NAME],
                capture_output=True,
                text=True,
                check=False,
            )
            (out / "container.log").write_text(log.stdout + log.stderr)
            subprocess.run(
                ["docker", "stop", "-t", "30", DOCKER_NAME],
                capture_output=True,
                check=False,
            )
            subprocess.run(
                ["docker", "rm", "-f", DOCKER_NAME], capture_output=True, check=False
            )
        else:
            subprocess.run(
                [str(REPO / "scripts" / "stop.sh")],
                check=False,
                stdout=subprocess.DEVNULL,
            )

    (out / "phases.json").write_text(json.dumps(phases_log, indent=2))
    rows = analyze.load_rows(out / "frames.jsonl")
    sha = subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"],
        capture_output=True,
        text=True,
        cwd=REPO,
        check=False,
    ).stdout.strip()
    results = {
        "meta": {
            "label": args.label,
            "git": sha,
            "platform": platform.platform(),
            "machine": platform.machine(),
            "sensor_path": sensor_path,
            "runtime": f"docker:{args.docker}" if args.docker else "native",
            "gpu": not args.no_gpu,
            "encoder": encoder_from_log((out / "container.log").read_text())
            if args.docker
            else None,
            "env": extra_env,
            "ready_s": round(ready_s, 1) if ready_s is not None else None,
            "latency": latency,
            "warmup_ran": not args.skip_warmup,
            "smoke": args.smoke,
            "site": args.site,
            "sensor": args.sensor,
            "config": str(args.config) if args.config else None,
        },
        "phases": analyze.summarize(rows, phases_log),
    }
    (out / "results.json").write_text(json.dumps(results, indent=2))
    print(json.dumps(results["phases"], indent=2))
    print(f"[bench] results in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
