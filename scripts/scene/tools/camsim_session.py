"""Shared by the gate tools: the CamSim environment for a scene package or Cesium ion, and one headless CamSim
run (scripts/run.sh --detach) with a 30 Hz CIGI host, torn down by scripts/stop.sh."""

from __future__ import annotations

import contextlib
import os
import subprocess
import sys
from collections.abc import Iterator
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "scripts" / "bench"))
import run_bench as rb
from bench import scenario

Pose = scenario.Pose


def env_for(mode: str, pkg: Path | None) -> dict[str, str]:
    env = {"CAMSIM_SNAPSHOT_ENDPOINT_ENABLED": "1"}
    if mode == "package":
        env["CAMSIM_SCENE_DIR"] = str(pkg)
    return env


@contextlib.contextmanager
def camsim(env: dict[str, str], pose: Pose, offline: bool = False) -> Iterator[rb.Host]:
    """Start the CIGI host at `pose`, launch CamSim headless with `env`, wait for /ready and yield the host
    (set `host.pose` to fly). `offline` (macOS) runs CamSim under sandbox-exec with ports 80/443 blocked."""
    cmd = [str(REPO / "scripts" / "run.sh"), "--headless", "--local", "--detach"]
    if offline:
        env = dict(env, CAMSIM_SCENE_OFFLINE="1")
        cmd = ["sandbox-exec", "-f", str(Path(__file__).with_name("offline.sb")), *cmd]
    host = rb.Host()
    host.pose = pose
    host.thread.start()
    try:
        subprocess.run(cmd, env=dict(os.environ, **env), check=True, stdout=subprocess.DEVNULL)
        rb.wait_ready(REPO / ".cache" / "camsim.pid")
        yield host
    finally:
        subprocess.run([str(REPO / "scripts" / "stop.sh")], check=False, stdout=subprocess.DEVNULL)
        host.stop.set()
