"""Tests for scripts/parse_automation_report.py."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

import parse_automation_report as par

FIXTURES = Path(__file__).resolve().parent / "fixtures"
ALL_PASS = FIXTURES / "automation_all_pass.json"
WITH_FAIL = FIXTURES / "automation_with_fail.json"


def test_load_strips_utf8_bom(tmp_path: Path) -> None:
    src = json.loads(ALL_PASS.read_text(encoding="utf-8"))
    bom_path = tmp_path / "index.json"
    bom_path.write_bytes(b"\xef\xbb\xbf" + json.dumps(src).encode("utf-8"))
    loaded = par.load_report(bom_path)
    assert loaded["succeeded"] == 2


def test_summarize_all_pass_returns_no_failures() -> None:
    report = par.load_report(ALL_PASS)
    counts, failed, warned = par.summarize(report)
    assert failed == []
    assert len(warned) == 1
    assert counts["succeeded"] == 2
    assert counts["succeededWithWarnings"] == 1
    assert counts["failed"] == 0
    assert counts["total"] == 3


def test_summarize_flags_failed_test() -> None:
    report = par.load_report(WITH_FAIL)
    _, failed, _ = par.summarize(report)
    assert len(failed) == 1
    assert failed[0]["fullTestPath"] == "CamSim.Phase28.BrokenInvariant"


def test_summarize_flags_test_with_error_count_despite_success_state() -> None:
    """Defensive: errors > 0 forces failure even if state mistakenly says Success."""
    bogus = {
        "succeeded": 1,
        "failed": 0,
        "tests": [
            {"fullTestPath": "X.Y", "state": "Success", "errors": 1, "warnings": 0}
        ],
    }
    _, failed, _ = par.summarize(bogus)
    assert len(failed) == 1


def test_summarize_treats_inprocess_as_failure() -> None:
    bogus = {"tests": [{"fullTestPath": "X.Y", "state": "InProcess"}]}
    _, failed, _ = par.summarize(bogus)
    assert len(failed) == 1


def test_collect_messages_extracts_event_messages() -> None:
    report = par.load_report(WITH_FAIL)
    test = report["tests"][1]
    msgs = par.collect_messages(test, "error")
    assert msgs == ["Expected 42 but got 0 at HttpServerLifecycleTest.cpp:88"]


def test_main_exit_zero_on_all_pass(monkeypatch, capsys) -> None:
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    rc = par.main([str(ALL_PASS), "--quiet"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "failed=0" in out
    assert "total=3" in out


def test_main_exit_one_on_failure(monkeypatch, capsys) -> None:
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    rc = par.main([str(WITH_FAIL), "--quiet"])
    assert rc == 1
    assert "failed=1" in capsys.readouterr().out


def test_main_exit_two_when_report_missing(tmp_path: Path, capsys) -> None:
    missing = tmp_path / "absent.json"
    rc = par.main([str(missing), "--quiet"])
    assert rc == 2
    assert "missing" in capsys.readouterr().out


def test_main_accepts_directory(tmp_path: Path, monkeypatch, capsys) -> None:
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    (tmp_path / "index.json").write_text(ALL_PASS.read_text())
    rc = par.main([str(tmp_path), "--quiet"])
    assert rc == 0


def test_main_writes_to_step_summary(tmp_path: Path, monkeypatch) -> None:
    summary = tmp_path / "summary.md"
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(summary))
    rc = par.main([str(WITH_FAIL)])
    assert rc == 1
    body = summary.read_text(encoding="utf-8")
    assert "# UE5 Automation Report" in body
    assert "CamSim.Phase28.BrokenInvariant" in body
    assert "Expected 42 but got 0" in body


def test_main_emits_gha_annotation_for_failure(monkeypatch, capsys) -> None:
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    par.main([str(WITH_FAIL)])
    out = capsys.readouterr().out
    assert "::error title=UE5 test failed: CamSim.Phase28.BrokenInvariant::" in out


def test_fmt_seconds() -> None:
    assert par.fmt_seconds(8.12) == "8.1s"
    assert par.fmt_seconds(0.5) == "0.5s"
    assert par.fmt_seconds(75.4) == "1m15s"


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-v"]))
