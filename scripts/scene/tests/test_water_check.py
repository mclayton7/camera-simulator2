"""water_check.py: an empty sample must fail the gate, not pass it."""

from __future__ import annotations

import importlib.util
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "water_check", Path(__file__).resolve().parents[1] / "tools" / "water_check.py"
)
water_check = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(water_check)


def test_nothing_sampled_is_a_failure():
    report = {"zooms": {}}
    assert water_check.sampling_failures(report, 10, 12) == ["no open-water leaves sampled"]
    assert report["zooms_unsampled"] == [10, 11, 12]


def test_unsampled_zooms_are_listed_but_do_not_fail():
    report = {"zooms": {"11": {}}}
    assert water_check.sampling_failures(report, 10, 12) == []
    assert report["zooms_unsampled"] == [10, 12]
