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


def _twilight(ratios, zen_rg=0.3):
    """mean dict for twilight_colour_metrics from illuminant ratios (R/G, B/G) vs a grey daylight ground."""
    np = pytest.importorskip("numpy")
    mean = {"day": {"ground": np.array([1.0, 1.0, 1.0]), "zenith": np.array([0.5, 1.0, 1.6])}}
    for tag, (rg, bg) in ratios.items():
        mean[tag] = {"ground": np.array([rg, 1.0, bg]), "zenith": np.array([zen_rg, 1.0, 3.0])}
    return mean


def test_twilight_n9_passes_the_measured_medians_and_flags_magenta():
    bands = json.loads(nc.TWILIGHT_BANDS.read_text())
    med = {t: (b["rg"][1], b["bg"][1]) for t, b in bands["bins"].items()}
    good = nc.twilight_colour_metrics(_twilight(med), bands)
    assert good["pass"], good["checks"]
    # 6A with UE's default ozone, measured at sun -3: the illuminant x1.61 R/G, x1.93 B/G (magenta)
    magenta = dict(med, z0=(1.03, 1.44), m3=(1.61, 1.93))
    bad = nc.twilight_colour_metrics(_twilight(magenta, zen_rg=1.6), bands)
    assert not bad["pass"]
    assert not bad["checks"]["zenith not magenta (R/G <= 1) at 0, -3, -6 deg"]
    # warmer at 0 than at +3 (non-monotonic) fails
    warm0 = dict(med, z0=(0.9, 1.2))
    assert not nc.twilight_colour_metrics(_twilight(warm0), bands)["checks"]["0 deg bluer than +3 deg in both ratios"]


def test_implied_mired_round_trips_the_blackbody_table():
    bands = json.loads(nc.TWILIGHT_BANDS.read_text())
    bb = bands["blackbody_rel"]
    i = 300
    m_rg, m_bg = nc.implied_mired(bands, bb["rg"][i], bb["bg"][i])
    assert m_rg == pytest.approx(1e6 / bb["temps_k"][i], rel=1e-3) and m_bg == pytest.approx(1e6 / bb["temps_k"][i], rel=1e-3)


def test_linear_rgb_inverts_the_bt709_oetf():
    np = pytest.importorskip("numpy")
    x = np.array([0.001, 0.01, 0.1, 0.5, 0.79])
    assert np.allclose(nc.linear_rgb(nc.oetf709(x) * 255.0), x, rtol=1e-6, atol=1e-9)
