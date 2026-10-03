#!/usr/bin/env python3
"""Multi-instance capacity probe: N CamSim containers at once on one GPU host (docs/capacity-linux-rtx5080.md).

Each instance gets its own CIGI / response / health / stream ports and Cesium cache volume, and a 30 Hz CIGI host flying
the bench orbit (scenario._orbit over San Francisco, 3 km, phases staggered 13 s apart). After every instance is ready
(terrain gate open) and a warm-up, it records per-instance frame stats (CAMSIM_FRAME_STATS_PATH), GPU / encoder / VRAM
(nvidia-smi), host CPU (/proc/stat), container memory (docker stats) and the encoder each instance opened.

usage: uv run --with numpy scripts/bench/scale_bench.py IMAGE OUT N [N ...] [--warm-s 45] [--measure-s 90] [--ir]
  e.g. uv run --with numpy scripts/bench/scale_bench.py camsim:latest .cache/scale 1 2 3 4 5 6
Results: OUT/summary.txt (one line per N) and OUT/n<N>[_ir]/result.json, per-container logs alongside.
Containers are named camsim-scale-<i> (removed afterwards); volumes camsim-scale-data-<i> keep their tile caches.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import re
import socket
import statistics
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
from bench import scenario


def ports(i: int) -> dict[str, int]:
    return {
        "cigi": 9000 + 10 * i,
        "resp": 9001 + 10 * i,
        "health": 18080 + i,
        "stream": 6000 + 2 * i,
    }


class Host(threading.Thread):
    """30 Hz CIGI host frames for one instance: the bench orbit, phase-shifted per instance."""

    def __init__(self, i: int, ir: bool):
        super().__init__(daemon=True)
        self.i, self.ir = i, ir
        self.port = ports(i)["cigi"]
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.orbit = scenario._orbit(
            scenario.BASE_LAT, scenario.BASE_LON, 3000.0, 2000.0, 120.0
        )
        self.stop = threading.Event()

    def run(self) -> None:
        t0 = time.monotonic()
        frame = 0
        while not self.stop.is_set():
            t = time.monotonic() - t0 + 13.0 * self.i
            pose = self.orbit(t)
            if self.ir:
                pose = dataclasses.replace(pose, sensor_id=1)
            self.sock.sendto(
                scenario.host_datagram(frame, pose), ("127.0.0.1", self.port)
            )
            frame += 1
            time.sleep(max(0.0, t0 + frame / 30.0 - time.monotonic()))


def http_json(port: int, path: str):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=3) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read())
        except Exception:  # noqa: BLE001
            return None
    except Exception:  # noqa: BLE001
        return None


def cpu_sample() -> tuple[int, int]:
    f = Path("/proc/stat").read_text().split("\n")[0].split()[1:]
    v = list(map(int, f))
    idle = v[3] + v[4]
    return sum(v), idle


def pct(xs, p):
    xs = sorted(xs)
    if not xs:
        return float("nan")
    k = (len(xs) - 1) * p / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def rows(path: Path) -> list[dict]:
    if not path.exists():
        return []
    out = []
    for line in path.read_text().splitlines():
        if line.startswith("{"):
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return out


def run_n(
    image: str, out: Path, n: int, warm_s: float, measure_s: float, ir: bool
) -> dict:
    d = out / f"n{n}{'_ir' if ir else ''}"
    subprocess.run(["rm", "-rf", str(d)], check=False)
    names = [f"camsim-scale-{i}" for i in range(n)]
    for nm in [f"camsim-scale-{i}" for i in range(32)]:
        subprocess.run(["docker", "rm", "-f", nm], capture_output=True, check=False)
    hosts = []
    for i in range(n):
        idir = d / f"i{i}"
        idir.mkdir(parents=True)
        idir.chmod(0o777)
        p = ports(i)
        env = {
            "CAMSIM_CIGI_PORT": p["cigi"],
            "CAMSIM_CIGI_RESPONSE_PORT": p["resp"],
            "CAMSIM_HEALTH_HTTP_PORT": p["health"],
            "CAMSIM_MULTICAST_ADDR": "127.0.0.1",
            "CAMSIM_MULTICAST_PORT": p["stream"],
            "CAMSIM_FRAME_STATS_PATH": "/bench/frames.jsonl",
            "CAMSIM_DIS_ENABLED": "0",
        }
        if ir:
            env["CAMSIM_IR_PRESET"] = "mwir_cooled"
        cmd = [
            "docker",
            "run",
            "-d",
            "--name",
            names[i],
            "--init",
            "--network",
            "host",
            "--shm-size",
            "1g",
            "-v",
            f"{idir}:/bench",
            "-v",
            f"camsim-scale-data-{i}:/var/lib/camsim",
            "-v",
            "camsim-bench-cache:/home/camsim/.cache",
            "--gpus",
            "all",
        ]
        for k, v in env.items():
            cmd += ["-e", f"{k}={v}"]
        h = Host(i, ir)
        h.start()
        hosts.append(h)
        subprocess.run([*cmd, image], check=True, stdout=subprocess.DEVNULL)
    t_launch = time.time()
    ready = {}
    deadline = time.time() + 900
    while len(ready) < n and time.time() < deadline:
        for i in range(n):
            if i in ready:
                continue
            b = http_json(ports(i)["health"], "/ready")
            if b and b.get("status") == "ready" and b.get("terrain_ready"):
                ready[i] = time.time() - t_launch
        alive = subprocess.run(
            ["docker", "ps", "-q", "--filter", "name=camsim-scale-"],
            capture_output=True,
            text=True,
            check=False,
        ).stdout.split()
        if len(alive) < n:
            break
        time.sleep(2)
    result: dict = {"n": n, "ir": ir, "ready_s": ready}
    if len(ready) < n:
        result["error"] = f"only {len(ready)}/{n} ready"
    else:
        time.sleep(warm_s)
        smi = subprocess.Popen(
            [
                "nvidia-smi",
                "--query-gpu=utilization.gpu,utilization.encoder,memory.used,clocks.sm",
                "--format=csv,noheader,nounits",
                "-lms",
                "1000",
            ],
            stdout=subprocess.PIPE,
            text=True,
        )
        c0 = cpu_sample()
        tm0 = time.time()
        time.sleep(measure_s)
        c1 = cpu_sample()
        tm1 = time.time()
        smi.terminate()
        gl = [
            list(map(float, x.split(",")))
            for x in smi.communicate()[0].strip().splitlines()
            if x.count(",") == 3
        ]
        total, idle = c1[0] - c0[0], c1[1] - c0[1]
        result["cpu_pct_of_machine"] = 100.0 * (total - idle) / max(total, 1)
        result["gpu_util_p50"] = pct([g[0] for g in gl], 50)
        result["gpu_util_p95"] = pct([g[0] for g in gl], 95)
        result["enc_util_p50"] = pct([g[1] for g in gl], 50)
        result["vram_mib_max"] = max((g[2] for g in gl), default=float("nan"))
        result["sm_clock_p50"] = pct([g[3] for g in gl], 50)
        st = subprocess.run(
            [
                "docker",
                "stats",
                "--no-stream",
                "--format",
                "{{.Name}} {{.CPUPerc}} {{.MemUsage}}",
            ],
            capture_output=True,
            text=True,
            check=False,
        ).stdout
        result["docker_stats"] = [
            ln for ln in st.splitlines() if ln.startswith("camsim-scale-")
        ]
        inst = []
        for i in range(n):
            rs = [
                r
                for r in rows(d / f"i{i}" / "frames.jsonl")
                if tm0 <= r.get("t", 0) <= tm1
            ]
            if len(rs) < 2:
                inst.append({"i": i, "error": "no rows"})
                continue
            wall = [r["wall_ms"] for r in rs]
            em = rs[-1]["emitted"] - rs[0]["emitted"]
            dr = rs[-1]["dropped"] - rs[0]["dropped"]
            span = rs[-1]["t"] - rs[0]["t"]
            log = subprocess.run(
                ["docker", "logs", names[i]],
                capture_output=True,
                text=True,
                check=False,
            )
            m = re.search(
                r"FVideoEncoder: using encoder (\S+)", log.stdout + log.stderr
            )
            inst.append(
                {
                    "i": i,
                    "frames": len(rs),
                    "fps_render": len(rs) / span,
                    "fps_emitted": em / span,
                    "dropped": dr,
                    "wall_p50": pct(wall, 50),
                    "wall_p95": pct(wall, 95),
                    "wall_p99": pct(wall, 99),
                    "wall_max": max(wall),
                    "slow_frames_pct": 100.0 * sum(w > 40.0 for w in wall) / len(wall),
                    "gpu_ms_p50": pct([r["gpu_ms"] for r in rs], 50),
                    "game_ms_p50": pct([r["game_ms"] for r in rs], 50),
                    "render_ms_p50": pct([r["render_ms"] for r in rs], 50),
                    "encoder": m.group(1) if m else None,
                }
            )
        result["instances"] = inst
    for h in hosts:
        h.stop.set()
    for nm in names:
        with open(d / f"{nm}.log", "w") as log_file:
            subprocess.run(
                ["docker", "logs", nm],
                stdout=log_file,
                stderr=subprocess.STDOUT,
                check=False,
            )
        subprocess.run(["docker", "rm", "-f", nm], capture_output=True, check=False)
    (d / "result.json").write_text(json.dumps(result, indent=2))
    return result


def summary(r: dict) -> str:
    if "error" in r:
        return f"N={r['n']}: ERROR {r['error']}"
    ins = [x for x in r["instances"] if "error" not in x]
    return (
        f"N={r['n']}{' IR' if r['ir'] else ''}: fps_emitted min {min(x['fps_emitted'] for x in ins):.2f} "
        f"wall p95 max {max(x['wall_p95'] for x in ins):.1f} ms p99 max {max(x['wall_p99'] for x in ins):.1f} "
        f"slow>40ms max {max(x['slow_frames_pct'] for x in ins):.2f}% dropped {sum(x['dropped'] for x in ins)} | "
        f"GPU {r['gpu_util_p50']:.0f}/{r['gpu_util_p95']:.0f}% enc {r['enc_util_p50']:.0f}% VRAM {r['vram_mib_max']:.0f} MiB "
        f"CPU {r['cpu_pct_of_machine']:.0f}% | encoders {sorted({str(x['encoder']) for x in ins})} "
        f"gpu_ms p50 {statistics.median(x['gpu_ms_p50'] for x in ins):.2f} game_ms p50 {statistics.median(x['game_ms_p50'] for x in ins):.2f}"
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("out", type=Path)
    ap.add_argument("ns", type=int, nargs="+")
    ap.add_argument("--warm-s", type=float, default=45.0)
    ap.add_argument("--measure-s", type=float, default=90.0)
    ap.add_argument("--ir", action="store_true")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    for n in a.ns:
        r = run_n(a.image, a.out, n, a.warm_s, a.measure_s, a.ir)
        line = summary(r)
        print(line, flush=True)
        with open(a.out / "summary.txt", "a") as f:
            f.write(line + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
