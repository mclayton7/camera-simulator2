"""camsim_session.py: the Docker launch (package mount, offline isolation) and the offline socat bridge commands."""

from __future__ import annotations

import importlib.util
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "camsim_session", Path(__file__).resolve().parents[1] / "tools" / "camsim_session.py"
)
cs = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(cs)


def _env(cmd: list[str]) -> dict[str, str]:
    return dict(cmd[i + 1].split("=", 1) for i, c in enumerate(cmd) if c == "-e")


def test_package_is_mounted_read_only_and_scene_dir_points_inside():
    cmd = cs.docker_run_cmd("camsim:x", cs.env_for("package", Path("/mnt/pendleton")), offline=False)
    assert "/mnt/pendleton:/data/scene/pendleton:ro" in cmd
    env = _env(cmd)
    assert env["CAMSIM_SCENE_DIR"] == "/data/scene/pendleton"
    assert env["CAMSIM_SNAPSHOT_ENDPOINT_ENABLED"] == "1"
    assert cmd[cmd.index("--network") + 1] == "host"
    assert "CAMSIM_SCENE_OFFLINE" not in env
    assert cmd[-1] == "camsim:x"


def test_offline_has_no_network_and_sets_scene_offline(monkeypatch):
    monkeypatch.setenv("CAMSIM_CESIUM_ION_TOKEN", "secret")
    cmd = cs.docker_run_cmd("camsim:x", cs.env_for("package", Path("/p/pkg")), offline=True)
    env = _env(cmd)
    assert cmd[cmd.index("--network") + 1] == "none"
    assert env["CAMSIM_SCENE_OFFLINE"] == "1"
    assert "CAMSIM_CESIUM_ION_TOKEN" not in env  # nothing that could reach ion


def test_cwt_passes_the_ion_token_and_mounts_nothing(monkeypatch):
    monkeypatch.setenv("CAMSIM_CESIUM_ION_TOKEN", "t")
    cmd = cs.docker_run_cmd("camsim:x", cs.env_for("cwt", None), offline=False)
    assert _env(cmd)["CAMSIM_CESIUM_ION_TOKEN"] == "t"
    assert not any(":/data/scene/" in c for c in cmd)


def test_bridge_joins_camsims_namespace_and_forwards_health_and_cigi(tmp_path):
    sidecar, host = cs.bridge_cmds(tmp_path)
    assert sidecar[sidecar.index("--network") + 1] == f"container:{cs.DOCKER_NAME}"
    assert f"{tmp_path}:/sock" in sidecar
    inner = sidecar[-1]
    assert "TCP4:127.0.0.1:8080" in inner and "UDP4-SENDTO:127.0.0.1:8888" in inner
    assert host[0][1].startswith("TCP4-LISTEN:8080,bind=127.0.0.1") and host[0][2].endswith("http.sock")
    assert host[1][2].startswith("UDP4-RECV:8888,bind=127.0.0.1") and host[1][3].endswith("cigi.sock")


def test_log_path_follows_the_runtime():
    assert cs.log_path("camsim:x") == cs.DOCKER_LOG
    assert cs.log_path(None) == cs.NATIVE_LOG
    assert cs.NATIVE_LOG.name == "CamSimTest.log"
