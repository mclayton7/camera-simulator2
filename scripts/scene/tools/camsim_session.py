"""Shared by the gate tools: the CamSim environment for a scene package or Cesium ion, and one headless CamSim
run with a 30 Hz CIGI host: natively (scripts/run.sh --detach, torn down by scripts/stop.sh) or from a Docker image
(`docker`, the package bind-mounted read-only at /data/scene/<name>)."""

from __future__ import annotations

import contextlib
import os
import subprocess
import sys
import tempfile
import time
from collections.abc import Iterator
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO / "scripts" / "bench"))
import run_bench as rb
from bench import scenario

Pose = scenario.Pose

DOCKER_NAME = "camsim-scene"
DOCKER_VOLUMES = {"camsim-scene-data": "/var/lib/camsim", "camsim-scene-cache": "/home/camsim/.cache"}
# Offline Docker runs (--network none) reach CIGI and the health port through Unix sockets in a shared directory:
# a socat sidecar in CamSim's network namespace (itself without a route out) on one side, host socat on 127.0.0.1
# on the other, so the tools keep talking to 127.0.0.1 as for a native run.
SOCAT_IMAGE = "alpine/socat:1.8.0.3"
SIDECAR_NAME = "camsim-scene-bridge"
# The log of the last session: UE on macOS logs to ~/Library/Logs/<Project>, elsewhere to the project's
# Saved/Logs (scripts/run.sh); a Docker session writes `docker logs` here at teardown.
NATIVE_LOG = (
    Path.home() / "Library/Logs/CamSimTest/CamSimTest.log"
    if sys.platform == "darwin"
    else REPO / "unreal_project/CamSimTest/Saved/Logs/CamSimTest.log"
)
DOCKER_LOG = REPO / ".cache" / "camsim-scene-docker.log"


def log_path(docker: str | None = None) -> Path:
    return DOCKER_LOG if docker else NATIVE_LOG


def env_for(mode: str, pkg: Path | None) -> dict[str, str]:
    env = {"CAMSIM_SNAPSHOT_ENDPOINT_ENABLED": "1"}
    if mode == "package":
        env["CAMSIM_SCENE_DIR"] = str(pkg)
    return env


def docker_run_cmd(image: str, env: dict[str, str], offline: bool) -> list[str]:
    """`docker run` for CamSim: CAMSIM_SCENE_DIR (a host path) becomes a read-only mount at /data/scene/<name>;
    `offline` is --network none and CAMSIM_SCENE_OFFLINE=1, otherwise host networking as the bench."""
    env = dict(env, CAMSIM_MULTICAST_ADDR="127.0.0.1")  # as run.sh --local
    mounts = []
    if pkg := env.get("CAMSIM_SCENE_DIR"):
        inside = f"/data/scene/{Path(pkg).name}"
        mounts.append(f"{pkg}:{inside}:ro")
        env["CAMSIM_SCENE_DIR"] = inside
    if offline:
        env["CAMSIM_SCENE_OFFLINE"] = "1"
    elif os.environ.get("CAMSIM_CESIUM_ION_TOKEN"):
        env["CAMSIM_CESIUM_ION_TOKEN"] = os.environ["CAMSIM_CESIUM_ION_TOKEN"]
    return rb.docker_run_cmd(
        image, None, True, env, [], name=DOCKER_NAME, network="none" if offline else "host",
        volumes=DOCKER_VOLUMES, mounts=mounts,
    )


def bridge_cmds(sock_dir: Path) -> tuple[list[str], list[list[str]]]:
    """The offline bridge: the sidecar `docker run` and the host-side socat commands (health TCP, CIGI UDP)."""
    port = rb.HEALTH.rsplit(":", 1)[1]
    inner = (
        f"socat UNIX-LISTEN:/sock/http.sock,fork,mode=777 TCP4:127.0.0.1:{port} & "
        f"socat -u UNIX-RECV:/sock/cigi.sock,mode=777 UDP4-SENDTO:127.0.0.1:{rb.CIGI_PORT} & wait"
    )
    sidecar = ["docker", "run", "-d", "--name", SIDECAR_NAME, "--network", f"container:{DOCKER_NAME}",
               "-v", f"{sock_dir}:/sock", "--entrypoint", "sh", SOCAT_IMAGE, "-c", inner]  # fmt: skip
    # IPv4 address types: socat 1.8's generic UDP-RECV with bind= fails ("Address family for hostname not supported").
    host = [
        ["socat", f"TCP4-LISTEN:{port},bind=127.0.0.1,fork,reuseaddr", f"UNIX-CONNECT:{sock_dir}/http.sock"],
        ["socat", "-u", f"UDP4-RECV:{rb.CIGI_PORT},bind=127.0.0.1", f"UNIX-SENDTO:{sock_dir}/cigi.sock"],
    ]
    return sidecar, host


def _rm(*names: str) -> None:
    subprocess.run(["docker", "rm", "-f", *names], capture_output=True, check=False)


@contextlib.contextmanager
def _docker_session(image: str, env: dict[str, str], offline: bool) -> Iterator[None]:
    _rm(DOCKER_NAME, SIDECAR_NAME)
    procs: list[subprocess.Popen] = []
    with tempfile.TemporaryDirectory(prefix="camsim-bridge-") as sock_dir:
        try:
            subprocess.run(docker_run_cmd(image, env, offline), check=True, stdout=subprocess.DEVNULL)
            if offline:
                os.chmod(sock_dir, 0o777)
                sidecar, host = bridge_cmds(Path(sock_dir))
                subprocess.run(sidecar, check=True, stdout=subprocess.DEVNULL)
                socks = [Path(sock_dir, s) for s in ("http.sock", "cigi.sock")]
                deadline = time.time() + 30
                while not all(s.exists() for s in socks):
                    if time.time() > deadline:
                        sys.exit(f"offline bridge: {SIDECAR_NAME} made no sockets in {sock_dir}")
                    time.sleep(0.2)
                procs = [subprocess.Popen(c) for c in host]  # stderr shows: a dead bridge must not look like a slow start

            def alive() -> bool | None:
                if any(p.poll() is not None for p in procs):
                    sys.exit("offline bridge: a host-side socat exited (see its error above)")
                return rb.docker_alive(DOCKER_NAME)

            rb.wait_ready(None, alive_fn=alive)
            yield
        finally:
            for p in procs:
                p.terminate()
            DOCKER_LOG.parent.mkdir(parents=True, exist_ok=True)
            with DOCKER_LOG.open("w") as f:
                subprocess.run(["docker", "logs", DOCKER_NAME], stdout=f, stderr=subprocess.STDOUT, check=False)
            subprocess.run(["docker", "stop", "-t", "30", DOCKER_NAME], capture_output=True, check=False)
            _rm(DOCKER_NAME, SIDECAR_NAME)


@contextlib.contextmanager
def camsim(env: dict[str, str], pose: Pose, offline: bool = False, docker: str | None = None) -> Iterator[rb.Host]:
    """Start the CIGI host at `pose`, launch CamSim headless with `env`, wait for /ready and yield the host
    (set `host.pose` to fly). `offline`: natively (macOS) CamSim runs under sandbox-exec with ports 80/443
    blocked; with `docker` (an image) the container has --network none. Either sets CAMSIM_SCENE_OFFLINE=1."""
    # Linux keeps the last run's :8080 in TIME_WAIT for 60 s, and /ready stays unreachable until CamSim's bind
    # retry gets through: back-to-back sessions read ~60 s too slow without this
    rb.wait_port_free(int(rb.HEALTH.rsplit(":", 1)[1]))
    host = rb.Host()
    host.pose = pose
    host.thread.start()
    try:
        if docker:
            with _docker_session(docker, env, offline):
                yield host
            return
        cmd = [str(REPO / "scripts" / "run.sh"), "--headless", "--local", "--detach"]
        if offline:
            if sys.platform != "darwin":
                sys.exit("--offline needs macOS (sandbox-exec) or --docker IMAGE (--network none)")
            env = dict(env, CAMSIM_SCENE_OFFLINE="1")
            cmd = ["sandbox-exec", "-f", str(Path(__file__).with_name("offline.sb")), *cmd]
        try:
            subprocess.run(cmd, env=dict(os.environ, **env), check=True, stdout=subprocess.DEVNULL)
            rb.wait_ready(REPO / ".cache" / "camsim.pid")
            yield host
        finally:
            subprocess.run([str(REPO / "scripts" / "stop.sh")], check=False, stdout=subprocess.DEVNULL)
    finally:
        host.stop.set()
