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
    h = np.array([-5.0, -5.5, -12.0, -13.0])  # a 6.5 m jump between 30 m samples
    assert not cc.gate_steps(h, np.array([100.0, 100.0, 100.0, 100.0]))["pass"]
    assert cc.gate_steps(h, np.array([5000.0] * 4))["pass"]  # far from the old 3DEP edge: not gated


def test_waterline_offsets_and_gate():
    wet = np.arange(30) >= 12  # water from 12; open sea (100 m from land) is reached at 22
    rel = np.where(np.arange(30) < 10, 5.0, -2.0)  # terrain crosses local MSL at index 10
    assert cc.waterline(rel, wet, 10.0) == {"edge_index": 12, "crossing_index": 10, "offset_m": 20.0}
    assert cc.waterline_offsets([rel, np.full(30, -1.0)], [wet, wet], 10.0) == [20.0]  # no crossing: skipped
    far = np.where(np.arange(30) < 3, 5.0, -2.0)  # an early crossing (a pond) and a nearer one: nearest wins
    far[9:11] = 1.0
    assert cc.waterline(far, wet, 10.0)["crossing_index"] == 11
    assert cc.gate_waterline([10.0, 20.0, 40.0])["pass"]  # median 20
    assert not cc.gate_waterline([40.0, 50.0])["pass"]
    assert not cc.gate_waterline([])["pass"]


def _package(tmp_path, with_station=True):
    import json

    (tmp_path / "manifest.json").write_text("{}")
    if with_station:
        (tmp_path / "sea_level.json").write_text(json.dumps({"offset_m": 0.5}))
    return tmp_path


def test_offset_gate_without_sea_level_json(tmp_path, monkeypatch):
    monkeypatch.setattr(cc.Manifest, "load", classmethod(lambda cls, p: type("M", (), {"sea_level": None})()))
    r = cc.gate_offset(_package(tmp_path, False))
    assert not r["pass"] and r["reason"] == "no sea_level.json"


def test_offset_gate_compares_with_independent_computation(tmp_path, monkeypatch):
    sl = {"lon": -117.2571, "lat": 32.8669, "msl_above_navd88_m": 0.0}
    monkeypatch.setattr(cc.Manifest, "load", classmethod(lambda cls, p: type("M", (), {"sea_level": sl})()))
    monkeypatch.setattr(cc, "navd88_ellipsoid_height", lambda lon, lat: cc.egm96(lat, lon) + 0.504)
    r = cc.gate_offset(_package(tmp_path))
    assert r["pass"] and r["diff_m"] == pytest.approx(-0.004)
    monkeypatch.setattr(cc, "navd88_ellipsoid_height", lambda lon, lat: cc.egm96(lat, lon) + 0.9)
    assert not cc.gate_offset(_package(tmp_path))["pass"]


def test_offset_gate_fails_when_geoid_grid_unavailable(tmp_path, monkeypatch):
    sl = {"lon": -117.2571, "lat": 32.8669, "msl_above_navd88_m": 0.0}
    monkeypatch.setattr(cc.Manifest, "load", classmethod(lambda cls, p: type("M", (), {"sea_level": sl})()))

    def unavailable(lon, lat):
        raise RuntimeError("GEOID18 unavailable: x")

    monkeypatch.setattr(cc, "navd88_ellipsoid_height", unavailable)
    assert cc.gate_offset(_package(tmp_path))["reason"].startswith("GEOID18 unavailable")


def test_distance_to_land_is_along_the_transect():
    wet = np.array([0, 1, 1, 1, 0, 1], bool)
    assert cc.land_distance(wet, 10.0).tolist() == [0.0, 10.0, 20.0, 10.0, 0.0, 10.0]


def test_egm96_matches_camsim_grid_at_la_jolla():
    assert cc.egm96(32.8669, -117.2571) == pytest.approx(-35.4, abs=0.6)
