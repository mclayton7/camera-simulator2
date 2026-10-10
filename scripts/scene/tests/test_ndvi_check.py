"""ndvi_check.py: the gate logic (the CLI runs in Task 8 on a real package)."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path

import numpy as np
import shapely
from fake_sources import build_synthetic, fast_fits, ndvi_scene

_SPEC = importlib.util.spec_from_file_location(
    "ndvi_check", Path(__file__).resolve().parents[1] / "tools" / "ndvi_check.py"
)
ndvi_check = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(ndvi_check)

GOOD = {10: 0.6, 20: 0.4, 30: 0.35, 50: 0.1, 60: 0.05, 80: -0.2}


def test_class_order():
    assert ndvi_check.class_order_failures(GOOD) == []
    assert ndvi_check.class_order_failures({**GOOD, 10: 0.38})  # trees not above shrub/grass
    assert ndvi_check.class_order_failures({**GOOD, 60: 0.36})  # bare not below shrub/grass
    assert ndvi_check.class_order_failures({**GOOD, 80: 0.1})  # water not below 0
    assert (
        ndvi_check.class_order_failures({k: v for k, v in GOOD.items() if k != 50}) == []
    )  # a missing class is skipped


def test_too_few_classes_is_a_failure():
    assert ndvi_check.class_order_failures({80: -0.2}) == [
        "too few classes sampled (need tree cover, shrub or grass, built-up or bare)"
    ]


def test_seam_pairs_straddle_the_edge():
    sq = shapely.box(10.0, 10.0, 10.1, 10.1)
    ins, outs = ndvi_check.seam_pairs([sq], 500.0)
    assert ins.shape[0] == 2 and ins.shape[1] > 50 and ins.shape == outs.shape
    assert shapely.contains_xy(sq, *ins).all() and not shapely.contains_xy(sq, *outs).any()
    d_in = shapely.distance(sq.exterior, shapely.points(ins.T)) * 111320.0
    assert np.allclose(d_in, ndvi_check.INSIDE_M, rtol=0.05)


def test_seam_pairs_without_footprints_is_empty():
    ins, outs = ndvi_check.seam_pairs([], 500.0)
    assert ins.shape == (2, 0) and outs.shape == (2, 0)


def test_colourise_marks_nodata():
    rgb = ndvi_check.colourise(np.array([[np.nan, -0.2, 0.8]]))
    assert rgb.shape == (1, 3, 3) and rgb[0, 0].tolist() == list(ndvi_check.NODATA_RGB)
    assert rgb[0, 2, 1] > rgb[0, 2, 0]  # dense vegetation is green


def test_cli_on_a_synthetic_package(tmp_path, monkeypatch, capsys):
    fast_fits(monkeypatch)
    pkg, _, _ = build_synthetic(tmp_path, ndvi_scene(tmp_path / "src"))
    out = tmp_path / "overview.png"
    rc = ndvi_check.main([str(pkg), "--cache", str(tmp_path / "cache"), "--overview", str(out), "--step-m", "500"])
    report = json.loads(capsys.readouterr().out)
    assert rc == 1 and report["fails"] == [
        "too few classes sampled (need tree cover, shrub or grass, built-up or bare)"
    ]
    seam = report["seam"]  # NAIP fitted onto Sentinel-2: no step at its edge; unfitted, NAIP is ~10 % lower
    assert seam["pairs"] > 50 and seam["pairs"] == seam["candidates"]  # exact footprints: 250 m in is pure NAIP
    assert abs(seam["bias"]) <= ndvi_check.SEAM_MAX and seam["bias_unfitted"] is not None
    assert seam["bias_global"] is not None and abs(seam["bias_global"]) <= ndvi_check.SEAM_MAX  # no regional offset
    assert out.exists()
    # a point inside NAIP's 200 m feather isn't pure NAIP (a dilated footprint puts seam points there)
    from camsim_scene.cache import Cache
    from camsim_scene.manifest import Manifest
    from camsim_scene.pipeline import make_context

    m = Manifest.load(pkg / "manifest.json")
    st = ndvi_check.WorkerState(make_context(pkg, m, Cache(tmp_path / "cache")))
    lat = np.full(3, 10.1)
    lon = 10.0 + np.array([100.0, 600.0, -100.0]) / 111320.0  # in the feather, beyond it, outside NAIP
    assert ndvi_check.pure_reference(st, m.layers["ndvi"], report["zooms"][-1], lon, lat).tolist() == [
        False,
        True,
        False,
    ]


def test_cli_without_ndvi_exits_2(tmp_path, capsys):
    pkg, _, _ = build_synthetic(tmp_path)
    assert ndvi_check.main([str(pkg), "--cache", str(tmp_path / "cache")]) == 2
    assert "no NDVI layer" in capsys.readouterr().out
