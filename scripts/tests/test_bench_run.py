"""Launch helpers in run_bench.py: port and process checks."""

import os
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest
from bench import run_bench, scenario

NETSTAT = """Active Internet connections (including servers)
Proto Recv-Q Send-Q  Local Address          Foreign Address        (state)
tcp4       0      0  192.168.1.20.52344     93.184.216.34.8080     ESTABLISHED
tcp4       0      0  127.0.0.1.8080         127.0.0.1.52311        TIME_WAIT
"""


def test_port_busy_matches_the_local_address_only():
    outbound_only = "\n".join(NETSTAT.splitlines()[:3])
    assert not run_bench.port_busy(outbound_only, 8080)
    assert run_bench.port_busy(NETSTAT, 8080)
    assert not run_bench.port_busy(NETSTAT, 80)


def test_port_busy_reads_linux_style_addresses():
    linux = (
        "tcp        0      0 0.0.0.0:8080            0.0.0.0:*               LISTEN\n"
    )
    assert run_bench.port_busy(linux, 8080)


def test_camsim_alive_is_unknown_without_a_pid_file(tmp_path):
    assert run_bench.camsim_alive(tmp_path / "camsim.pid") is None


def test_camsim_alive_checks_the_pid(tmp_path):
    pid_file = tmp_path / "camsim.pid"
    pid_file.write_text(f"{os.getpid()}\n\n")
    assert run_bench.camsim_alive(pid_file) is True
    pid_file.write_text("999999\n\n")
    assert run_bench.camsim_alive(pid_file) is False


def test_wait_ready_gives_up_when_no_pid_file_appears(tmp_path, monkeypatch):
    monkeypatch.setattr(run_bench, "http_json", lambda path: None)
    with pytest.raises(SystemExit, match="pid file"):
        run_bench.wait_ready(tmp_path / "camsim.pid", pid_grace_s=0.0)


def test_sensor_path_is_read_from_metrics():
    text = '# HELP x\ncamsim_uptime_seconds 3\ncamsim_sensor_path{path="gpu"} 1\n'
    assert run_bench.sensor_path_from_metrics(text) == "gpu"
    assert run_bench.sensor_path_from_metrics("camsim_uptime_seconds 3\n") is None


def test_sensor_path_flag_is_gone():
    # 3B.2: the GPU sensor graph is the only path; meta.sensor_path is still recorded.
    parser = run_bench.build_parser()
    assert parser.parse_args(["--label", "x"]).label == "x"
    with pytest.raises(SystemExit):
        parser.parse_args(["--label", "x", "--sensor-path", "gpu"])


def test_docker_run_cmd_mounts_out_and_passes_gpu_and_env(tmp_path):
    cmd = run_bench.docker_run_cmd(
        "camsim:x", tmp_path, True, {"CAMSIM_ENCODER": "libx264"}, ["-trace=cpu"]
    )
    assert cmd[:2] == ["docker", "run"]
    assert f"{tmp_path}:/bench" in cmd
    assert "--gpus" in cmd and "CAMSIM_ENCODER=libx264" in cmd
    assert cmd[-2:] == ["camsim:x", "-trace=cpu"]
    assert "--gpus" not in run_bench.docker_run_cmd("camsim:x", tmp_path, False, {}, [])


def test_docker_run_cmd_takes_name_network_volumes_and_mounts(tmp_path):
    cmd = run_bench.docker_run_cmd(
        "img", None, True, {}, [], name="camsim-scene", network="none",
        volumes={"v": "/var/lib/camsim"}, mounts=["/pkg:/data/scene/p:ro"],
    )
    assert cmd[cmd.index("--name") + 1] == "camsim-scene"
    assert cmd[cmd.index("--network") + 1] == "none"
    assert "v:/var/lib/camsim" in cmd and "/pkg:/data/scene/p:ro" in cmd
    assert not any(c.endswith(":/bench") for c in cmd)
    assert "camsim-bench-data:/var/lib/camsim" not in cmd


def test_docker_gpu_flag_follows_camsim_docker_gpu(tmp_path, monkeypatch):
    monkeypatch.setenv("CAMSIM_DOCKER_GPU", "--device nvidia.com/gpu=all")
    cmd = run_bench.docker_run_cmd("img", tmp_path, True, {}, [])
    assert "--gpus" not in cmd
    assert cmd[cmd.index("--device") + 1] == "nvidia.com/gpu=all"


def test_volume_flag_parses():
    args = run_bench.build_parser().parse_args(
        ["--label", "x", "--docker", "img", "--volume", "/a:/b:ro"]
    )
    assert args.volume == ["/a:/b:ro"]

def test_encoder_and_latency_are_parsed():
    log = "LogCamSim: FVideoEncoder: using encoder h264_nvenc\n"
    assert run_bench.encoder_from_log(log) == "h264_nvenc"
    assert run_bench.encoder_from_log("nothing") is None
    metrics = (
        'camsim_frame_latency_ms{quantile="0.5"} 13.875\n'
        'camsim_frame_latency_ms{quantile="0.99"} 38.4\n'
        "camsim_uptime_seconds 3\n"
    )
    assert run_bench.latency_from_metrics(metrics) == {
        "frame_latency_ms_p50": 13.875,
        "frame_latency_ms_p99": 38.4,
    }


def test_no_gpu_and_env_flags_parse():
    args = run_bench.build_parser().parse_args(
        ["--label", "x", "--docker", "img", "--no-gpu", "--env", "A=1", "--env", "B=2"]
    )
    assert args.docker == "img" and args.no_gpu and args.env == ["A=1", "B=2"]


def test_config_is_mounted_over_the_image_config(tmp_path):
    cfg = tmp_path / "c.yaml"
    cfg.write_text("capture_width: 1920\n")
    cmd = run_bench.docker_run_cmd("img", tmp_path, True, {}, [], cfg)
    assert f"{cfg}:/opt/camsim/CamSimTest/camsim_config.yaml:ro" in cmd


def test_with_sensor_sets_the_waveband():
    from bench import scenario

    pose = scenario.build_phases(smoke=True)[0].pose_at(0.0)
    assert run_bench.with_sensor(pose, "ir").sensor_id == 1
    assert run_bench.with_sensor(pose, "eo").sensor_id == 0


def test_filter_phases_keeps_warmup_and_named_measured_phases():
    phases = scenario.build_phases(site=scenario.SITES["pendleton"])
    out = run_bench.filter_phases(phases, ["coast_pass"], skip_warmup=False)
    assert [p.name for p in out] == ["warmup", "coast_pass"]


def test_filter_phases_none_means_all_and_skip_warmup_drops_it():
    phases = scenario.build_phases()
    assert run_bench.filter_phases(phases, None, False) == phases
    assert [p.name for p in run_bench.filter_phases(phases, None, True)] == [
        "orbit", "slew", "low_pass", "far_origin"
    ]


def test_filter_phases_rejects_unknown_names_with_the_valid_list():
    with pytest.raises(SystemExit, match="unknown phase 'dense_pas'.*orbit"):
        run_bench.filter_phases(scenario.build_phases(), ["dense_pas"], False)


def test_filter_shots_selects_and_rejects():
    shots = scenario.build_shots()
    assert [s.name for s in run_bench.filter_shots(shots, ["horizon"])] == ["horizon"]
    assert run_bench.filter_shots(shots, None) == shots
    with pytest.raises(SystemExit, match="unknown shot 'nope'"):
        run_bench.filter_shots(shots, ["nope"])


def test_phases_and_shots_flags_parse_as_lists():
    a = run_bench.build_parser().parse_args(
        ["--label", "x", "--phases", "orbit,slew", "--shots", "horizon"]
    )
    assert a.phases == ["orbit", "slew"] and a.shots == ["horizon"]
    b = run_bench.build_parser().parse_args(["--label", "x"])
    assert b.phases is None and b.shots is None


FIXTURES = Path(__file__).parent / "fixtures"


def _ran(stdout, rc=0):
    return lambda *a, **k: SimpleNamespace(returncode=rc, stdout=stdout, stderr="")


def test_nvidia_smi_sums_gpus_and_handles_absence():
    assert run_bench.nvidia_smi_mib(_ran("1024\n2048\n")) == 3072.0
    assert run_bench.nvidia_smi_mib(_ran("", rc=127)) is None

    def boom(*a, **k):
        raise FileNotFoundError("nvidia-smi")

    assert run_bench.nvidia_smi_mib(boom) is None


def test_footprint_parses_the_captured_camsim_output():
    text = (FIXTURES / "footprint_camsim.txt").read_text()
    # IOAccelerator 1694 + IOAccelerator (graphics) 1113 (once) + Owned physical footprint (unmapped) (graphics) 2570
    assert run_bench.footprint_mib("123", _ran(text)) == 5377.0


def test_footprint_units_and_failures():
    table = (
        "  Dirty      Clean  Reclaimable    Regions    Category\n"
        "    ---        ---          ---        ---    ---\n"
        f"1.5 GB        0 B          0 B         12    {run_bench.FOOTPRINT_CATEGORY_PREFIX}\n"
    )
    assert run_bench.footprint_mib("1", _ran(table)) == 1536.0
    assert run_bench.footprint_mib("1", _ran("no such process", rc=1)) is None

    def boom(*a, **k):
        raise PermissionError("footprint")

    assert run_bench.footprint_mib("1", boom) is None


def test_footprint_pid_file_guard_and_timeouts(tmp_path):
    empty = tmp_path / "pid"
    empty.write_text("")
    assert run_bench._footprint_of_pid_file(empty) is None
    assert run_bench._footprint_of_pid_file(tmp_path / "missing") is None

    def hung(*a, **k):
        raise subprocess.TimeoutExpired("x", 10)

    assert run_bench.footprint_mib("1", hung) is None
    assert run_bench.nvidia_smi_mib(hung) is None


def test_gpu_mem_flag_parses():
    assert run_bench.build_parser().parse_args(["--label", "x", "--gpu-mem"]).gpu_mem
