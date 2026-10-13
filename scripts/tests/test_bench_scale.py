"""Multi-instance capacity probe: docker command, extra env / mounts, the dense shuttle path (REALISM R3/R4 on Linux)."""

import pytest
from bench import scale_bench, scenario


def test_docker_cmd_carries_ports_extra_env_and_mounts(tmp_path):
    cmd = scale_bench.docker_cmd(
        "camsim:x", tmp_path, 2, ir=False,
        env={"CAMSIM_SCENE_DIR": "/data/scene/p"}, mounts=["/mnt/p:/data/scene/p:ro"],
    )
    assert cmd[-1] == "camsim:x"
    assert ["--name", "camsim-scale-2"] == cmd[cmd.index("--name"):cmd.index("--name") + 2]
    assert "CAMSIM_CIGI_PORT=9020" in cmd and "CAMSIM_HEALTH_HTTP_PORT=18082" in cmd
    assert "CAMSIM_SCENE_DIR=/data/scene/p" in cmd
    i = cmd.index("/mnt/p:/data/scene/p:ro")
    assert cmd[i - 1] == "-v"
    assert f"{tmp_path}:/bench" in cmd


def test_extra_env_overrides_the_defaults(tmp_path):
    cmd = scale_bench.docker_cmd("img", tmp_path, 0, ir=False, env={"CAMSIM_DIS_ENABLED": "1"}, mounts=[])
    assert "CAMSIM_DIS_ENABLED=1" in cmd and "CAMSIM_DIS_ENABLED=0" not in cmd


def test_parse_env_rejects_a_missing_equals():
    assert scale_bench.parse_env(["A=1", "B=x=y"]) == {"A": "1", "B": "x=y"}
    with pytest.raises(SystemExit):
        scale_bench.parse_env(["A"])


def test_dense_shuttle_runs_the_pass_out_and_back():
    leg = scale_bench.DENSE_LEG_S
    out = scale_bench.dense_shuttle(10.0)
    assert (out.lat, out.lon) == pytest.approx((scenario._dense_pass(10.0).lat, scenario._dense_pass(10.0).lon))
    assert out.yaw == pytest.approx(scenario.DENSE["bearing_deg"])
    back = scale_bench.dense_shuttle(2 * leg - 10.0)
    assert (back.lat, back.lon) == pytest.approx((out.lat, out.lon))
    assert back.yaw == pytest.approx((scenario.DENSE["bearing_deg"] + 180.0) % 360.0)
    again = scale_bench.dense_shuttle(2 * leg + 10.0)
    assert (again.lat, again.lon) == pytest.approx((out.lat, out.lon))
