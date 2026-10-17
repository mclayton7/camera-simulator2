"""Unit tests for scripts/night_check.py's summary (ROADMAP 6A acceptance): one complete N1-N8 table across split runs."""

import json

import pytest

nc = pytest.importorskip("night_check")


def test_summary_merges_earlier_gate_jsons(tmp_path):
    (tmp_path / "N1.json").write_text(json.dumps(
        {"gate": "N1", "pass": True, "detail": "day ok", "evaluated_utc": "2026-10-16T18:00:00Z", "commit": "abc1234"}))
    (tmp_path / "N6.json").write_text(json.dumps({"gate": "N6", "pass": False, "detail": "exit 1"}))
    now = nc.gate(tmp_path, "N4", True, {}, ">= 20", "130 of 130")
    gates = nc.merged_gates(tmp_path, [now])
    assert [g["gate"] for g in gates] == [f"N{i}" for i in range(1, 9)]
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
