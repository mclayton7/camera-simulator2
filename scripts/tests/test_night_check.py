"""Unit tests for scripts/night_check.py's summary (ROADMAP 6A acceptance): one complete N1-N9 table across split runs."""

import json

import pytest

nc = pytest.importorskip("night_check")


def test_summary_merges_earlier_gate_jsons(tmp_path):
    (tmp_path / "N1.json").write_text(json.dumps(
        {"gate": "N1", "pass": True, "detail": "day ok", "evaluated_utc": "2026-10-16T18:00:00Z", "commit": "abc1234"}))
    (tmp_path / "N6.json").write_text(json.dumps({"gate": "N6", "pass": False, "detail": "exit 1"}))
    now = nc.gate(tmp_path, "N4", True, {}, ">= 20", "130 of 130")
    gates = nc.merged_gates(tmp_path, [now])
    assert [g["gate"] for g in gates] == [f"N{i}" for i in range(1, 10)]
    md = nc.summary_markdown(gates, {"N4"})
    rows = {line.split(" | ")[0].strip("| "): line for line in md.splitlines()[2:]}
    assert "PASS" in rows["N1"] and "earlier invocation" in rows["N1"] and "abc1234" in rows["N1"]
    assert "FAIL" in rows["N6"]
    assert "PASS" in rows["N4"] and "earlier invocation" not in rows["N4"]
    assert "not run" in rows["N2"] and "n/a" in rows["N2"]
    assert json.loads((tmp_path / "N4.json").read_text())["commit"]


def test_this_invocation_wins_over_an_older_json(tmp_path):
    (tmp_path / "N2.json").write_text(json.dumps({"gate": "N2", "pass": False, "detail": "old"}))
    now = nc.gate(tmp_path, "N2", True, {}, "t", "new")
    gates = {g["gate"]: g for g in nc.merged_gates(tmp_path, [now])}
    assert gates["N2"]["detail"] == "new" and gates["N2"]["pass"] is True


def test_twilight_colour_flags_magenta_and_passes_blue():
    np = pytest.importorskip("numpy")
    day = np.array([0.40, 0.36, 0.27])   # warm ground in daylight
    # 6A before the ozone fix (measured at sun -3): magenta ground and zenith
    bad = nc.twilight_colour_metrics(day, np.array([0.30, 0.15, 0.24]), np.array([0.26, 0.15, 0.34]))
    assert not bad["pass"] and not bad["checks"]["zenith R/G <= 1 (not magenta)"]
    assert bad["illum_rg"] > 1.0 and bad["illum_bg"] > 1.0
    good = nc.twilight_colour_metrics(day, np.array([0.13, 0.14, 0.17]), np.array([0.10, 0.125, 0.28]))
    assert good["pass"], good["checks"]
    red = nc.twilight_colour_metrics(day, np.array([0.30, 0.20, 0.10]), np.array([0.10, 0.125, 0.28]))
    assert not red["pass"]


def test_linear_rgb_inverts_the_bt709_oetf():
    np = pytest.importorskip("numpy")
    x = np.array([0.001, 0.01, 0.1, 0.5, 0.79])
    assert np.allclose(nc.linear_rgb(nc.oetf709(x) * 255.0), x, rtol=1e-6, atol=1e-9)
