"""tools/detail_check.py (REALISM R5 gates 2 and 3)."""

from __future__ import annotations

import importlib.util
from pathlib import Path

import numpy as np
import shapely
from PIL import Image

from camsim_scene.layers.landcover import tile_filename
from camsim_scene.tiling import Region

_SPEC = importlib.util.spec_from_file_location(
    "detail_check", Path(__file__).resolve().parents[1] / "tools" / "detail_check.py"
)
dc = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(dc)

REGIONS = [
    Region.from_bounds("bbox", (10.0, 10.0, 10.5, 10.5), {"terrain": 8, "imagery": 8}),
    Region.from_bounds("detail:p", (10.0, 10.0, 10.15, 10.15), {"terrain": 11, "imagery": 11}),
    Region.from_bounds("detail_margin:p", (9.97, 9.97, 10.18, 10.18), {"terrain": 3, "imagery": 11}),
]


def test_parse_tile():
    assert dc.parse_tile("imagery/9/541/284.jpg") == ("imagery", 9, 541, 284)
    assert dc.parse_tile("terrain/11/2161/1137.terrain") == ("terrain", 11, 2161, 1137)
    assert dc.parse_tile("landcover/+10.00_+10.00.png") is None


def test_scope_violations():
    ok = {"imagery/9/540/284.jpg": "a", "terrain/9/540/284.terrain": "b", "imagery/8/270/142.jpg": "c"}
    assert dc.scope_violations(ok, REGIONS) == []
    sibling = {"imagery/9/541/285.jpg": "a"}  # outside the margin, but its z8 parent was expanded: a legal sibling
    assert dc.scope_violations(sibling, REGIONS) == []
    bad = {"imagery/10/1083/571.jpg": "a"}  # its z9 parent (541, 285) misses the margin: must not exist
    assert dc.scope_violations(bad, REGIONS) == ["imagery/10/1083/571.jpg"]


def test_identity_counts_only_tiles_inside():
    inner = shapely.box(10.0, 10.0, 10.2, 10.2)  # holds z11 x 2162-2163, y 1138 (0.0879-degree tiles)
    a = {"imagery/11/2162/1138.jpg": "x", "imagery/11/2163/1138.jpg": "y", "imagery/11/2170/1138.jpg": "z"}
    ref = {"imagery/11/2162/1138.jpg": "x", "imagery/11/2163/1138.jpg": "DIFF", "imagery/11/2170/1138.jpg": "q"}
    compared, mism = dc.identity(a, ref, "imagery", 11, inner)
    assert compared == 2 and mism == ["imagery/11/2163/1138.jpg"]


def test_water_near(tmp_path):
    lc = tmp_path / "landcover"
    lc.mkdir()
    codes = np.full((600, 600), 10, np.uint8)
    codes[:10, :10] = 80  # north-west corner of tile (200, 200) = lat [10.0, 10.05), lon [10.0, 10.05)
    Image.fromarray(codes).save(lc / tile_filename(200, 200))
    assert dc.water_near(lc, (10.0, 10.04, 10.002, 10.0499))
    assert not dc.water_near(lc, (10.03, 10.0, 10.04, 10.01))
    assert not dc.water_near(lc, (20.0, 20.0, 20.01, 20.01))  # no tile: no data, not water


def test_main_on_a_built_detail_package(tmp_path, monkeypatch, capsys):
    import json
    import sys

    from test_detail_build import built

    pkg = built(tmp_path)
    monkeypatch.setattr(sys, "argv", ["detail_check.py", str(pkg), "--reference", str(pkg), "--shrink-km", "0.5"])
    assert dc.main() == 0
    rep = json.loads(capsys.readouterr().out)
    assert rep["scope"]["tiles_outside"] == 0 and rep["scope"]["footprints_outside"] == []
    assert rep["identity"]["imagery"]["compared"] > 0 and rep["identity"]["terrain"]["compared"] > 0


def _tool(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().parents[1] / "tools" / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_render_check_shot_sets(monkeypatch):
    monkeypatch.syspath_prepend(str(Path(__file__).resolve().parents[1] / "tools"))
    rc = _tool("render_check")
    main, beaufort = rc.shot_plan("detail")
    assert set(main) == {"edge_inland", "edge_coast", "outside_high", "outside_low"} and beaufort == {}
    main, beaufort = rc.shot_plan("r0")
    assert "nadir_2km" in main and "coast_waves" not in main and beaufort == {"5": ["coast_waves"]}


def test_edge_shots_crop_parsing():
    es = _tool("edge_shots")
    assert es.parse_crops([]) == es.CROPS
    assert es.parse_crops(["ne:14:-117.3:33.46:-117.18:33.56"]) == {"ne": (14, -117.3, 33.46, -117.18, 33.56)}
