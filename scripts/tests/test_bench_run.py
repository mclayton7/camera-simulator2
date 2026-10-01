"""Launch helpers in run_bench.py: port and process checks."""

import os

import pytest
from bench import run_bench

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
