import logging
from pathlib import Path

from camsim_hitl.__main__ import main
from camsim_hitl.config import Config, load_config, parse_hostport

HITL = Path(__file__).resolve().parents[1]


def test_example_config_loads_and_matches_defaults():
    cfg = load_config(HITL / "hitl.example.toml")
    d = Config()
    # the example documents the defaults; only the "example" keys differ
    assert cfg.cigi == d.cigi
    assert cfg.truth == d.truth
    assert cfg.terrain_blend == d.terrain_blend
    assert cfg.weather == d.weather
    assert cfg.time == d.time
    assert cfg.mavlink.links["payload"].url == "udpin:0.0.0.0:13280"
    assert cfg.gimbal.enabled and cfg.gimbal.link == "payload"
    assert cfg.camera.params[1].role_values == {"eo": 0, "ir": 1}
    assert cfg.camera.zoom == d.camera.zoom
    assert cfg.base_dir == str(HITL)


def test_unknown_keys_warn(tmp_path, caplog):
    p = tmp_path / "c.toml"
    p.write_text('[cigi]\ncamsim_port = 9999\nbogus = 1\n[truth]\nmax_extrapolation_s = 1\n')
    cfg = load_config(p)
    assert cfg.cigi.camsim_port == 9999
    assert isinstance(cfg.truth.max_extrapolation_s, float)
    assert "bogus" in caplog.text


def test_parse_hostport():
    assert parse_hostport("0.0.0.0:8889") == ("0.0.0.0", 8889)
    assert parse_hostport("8889") == ("0.0.0.0", 8889)
    assert parse_hostport(":8889", "127.0.0.1") == ("127.0.0.1", 8889)


def test_check_config_cli(capsys):
    root = logging.getLogger()
    saved = root.handlers[:], root.level
    try:
        assert main(["--config", str(HITL / "hitl.example.toml"), "--check-config"]) == 0
        assert "camsim_port=8888" in capsys.readouterr().out
    finally:
        root.handlers[:], _ = saved
        root.setLevel(saved[1])
