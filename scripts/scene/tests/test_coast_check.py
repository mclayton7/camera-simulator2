"""coast_check.py: the pure gate functions on synthetic arrays, and the EGM96 read against CamSim's grid."""

from __future__ import annotations

import importlib.util
from pathlib import Path

import numpy as np
import pytest

_SPEC = importlib.util.spec_from_file_location(
    "coast_check", Path(__file__).resolve().parents[1] / "tools" / "coast_check.py"
)
cc = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(cc)


def test_transect_steps_along_bearing():
    lon, lat = cc.transect(33.0, -117.0, 270.0, 10.0, 100.0)
    assert len(lon) == 11 and np.allclose(lat, 33.0) and lon[-1] < lon[0]


def test_offshore_gate():
    rel = np.array([-2.0] * 99 + [0.1])  # one sample of 100 within 0.5 m of the sea
    assert cc.gate_offshore(rel, np.full(100, 500.0))["pass"]  # 99 % rule
    assert not cc.gate_offshore(np.array([-0.2] * 10), np.full(10, 500.0))["pass"]
    assert cc.gate_offshore(np.array([0.0] * 10), np.full(10, 50.0))["n"] == 0  # < 100 m from land: not counted
    assert not cc.gate_offshore(np.array([0.0] * 10), np.full(10, 50.0))["pass"]  # nothing sampled is a failure


def test_step_gate():
    h = np.array([-5.0, -5.5, -12.0, -13.0])  # a 7 m jump between 30 m samples
    assert not cc.gate_steps(h, np.array([100.0, 100.0, 100.0, 100.0]))["pass"]
    assert cc.gate_steps(h, np.array([5000.0] * 4))["pass"]  # far from the old 3DEP edge: not gated


def test_waterline_offsets_and_gate():
    rel = np.array([5.0, 3.0, 1.0, -1.0, -2.0, -2.0])  # terrain crosses local MSL at index 3
    wet = np.array([0, 0, 0, 0, 0, 1], bool)  # WorldCover water from index 5
    assert cc.waterline_offsets([rel, np.full(6, -1.0)], [wet, wet], 10.0) == [20.0]  # second never crosses: skipped
    assert cc.gate_waterline([10.0, 20.0, 40.0])["pass"]  # median 20
    assert not cc.gate_waterline([40.0, 50.0])["pass"]
    assert not cc.gate_waterline([])["pass"]


def test_distance_to_land_is_along_the_transect():
    wet = np.array([0, 1, 1, 1, 0, 1], bool)
    assert cc.land_distance(wet, 10.0).tolist() == [0.0, 10.0, 20.0, 10.0, 0.0, 10.0]


def test_egm96_matches_camsim_grid_at_la_jolla():
    assert cc.egm96(32.8669, -117.2571) == pytest.approx(-35.4, abs=0.6)
