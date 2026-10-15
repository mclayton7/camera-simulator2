import json

import numpy as np
from fake_sources import build_synthetic, source
from PIL import Image
from rasters import write_geotiff
from test_buildings_layer import ROWS, _scene
from test_pipeline import FEATURES, _lc_setup

from camsim_scene import footprints as F
from camsim_scene import pipeline
from camsim_scene.manifest import AssetRecord, SourceRecord


def _with_buildings(tmp_path, monkeypatch, features=None, key="k1"):
    pkg, m, build = _lc_setup(tmp_path, monkeypatch, features)
    m.sources.append(
        SourceRecord("overture_buildings", "overture_buildings", "overture", "2026-09-23.1", "ODbL-1.0",
                     "© Overture Maps Foundation", {}, [AssetRecord("snap", "overture://snap", "cd" * 32, 1)])
    )
    m.layers["buildings"] = {"priorities": ["overture_buildings"], "min_area_m2": 10.0, "rules": "2"}
    m.layers["landcover"]["buildings"] = {"code": 230, "cell_factor": 8, "min_cells": 32, "rules": F.MASK_RULES_VERSION}
    mask = np.zeros((F.MASK_PX, F.MASK_PX), bool)
    mask[800:1600, 800:1600] = True  # land-cover cells 100..199 of tile (200, 200)
    rdir = tmp_path / "fp"
    F.save_raster({(200, 200): np.packbits(mask, axis=1)}, rdir, "k")
    state = {"key": key, "calls": 0}

    def fake_raster(pkg_, m_, ctx_, wst=None, jobs=1):
        state["calls"] += 1
        return rdir, state["key"], 0.0

    monkeypatch.setattr(pipeline.buildings_layer, "footprint_raster", fake_raster)
    return pkg, m, build, state


def test_buildings_burn_230_with_legend_and_credit(tmp_path, monkeypatch):
    pkg, _, build, _ = _with_buildings(tmp_path, monkeypatch)
    build()
    index = json.loads((pkg / "landcover" / "index.json").read_text())
    assert index["codes"] == {"230": "building"}
    assert len(index["credits"]) == 1 and index["credits"][0].startswith("© Overture Maps Foundation")
    codes = np.array(Image.open(pkg / "landcover" / "+10.00_+10.00.png"))
    assert (codes[100:200, 100:200] == 230).all() and (codes == 230).sum() == 100 * 100
    assert "© Overture Maps Foundation" in (pkg / "landcover" / "ATTRIBUTION.txt").read_text()


def test_buildings_and_features_share_the_legend(tmp_path, monkeypatch):
    pkg, _, build, _ = _with_buildings(tmp_path, monkeypatch, FEATURES)
    build()
    index = json.loads((pkg / "landcover" / "index.json").read_text())
    assert index["codes"] == {"200": "paved_asphalt", "230": "building"}
    assert len(index["credits"]) == 2  # OSM, then Overture


def test_buildings_inputs_rebuild_and_skip(tmp_path, monkeypatch):
    _, m, build, state = _with_buildings(tmp_path, monkeypatch)
    assert build().built == 2
    assert build().skipped == 2
    state["key"] = "k2"  # the kept buildings changed (snapshot, terrain, rules)
    assert build().built == 2
    m.layers["buildings"]["min_area_m2"] = 20.0  # the buildings settings hash
    assert build().built == 2


def test_no_buildings_never_reads_footprints(tmp_path, monkeypatch):
    pkg, _, build = _lc_setup(tmp_path, monkeypatch, FEATURES)

    def boom(*a, **k):
        raise AssertionError("footprint_raster called without [buildings]")

    monkeypatch.setattr(pipeline.buildings_layer, "footprint_raster", boom)
    build()
    assert json.loads((pkg / "landcover" / "index.json").read_text())["codes"] == {"200": "paved_asphalt"}


def test_r3_manifest_with_buildings_but_no_burn_settings_is_untouched(tmp_path, monkeypatch):
    pkg, m, build, state = _with_buildings(tmp_path, monkeypatch)
    del m.layers["landcover"]["buildings"]  # an R3 manifest
    build()
    assert state["calls"] == 0
    assert "codes" not in json.loads((pkg / "landcover" / "index.json").read_text())


def test_end_to_end_burn_and_mask_agree(tmp_path, monkeypatch):
    # the synthetic buildings scene plus a land-cover source; the COG reader returns tree cover (10) everywhere and the
    # cut runs in-process so the patched reader applies (synthetic sources are not WorldCover COGs)
    scene = _scene(tmp_path, ROWS)
    write_geotiff(tmp_path / "src" / "wc.tif", np.full((10, 10), 10, np.uint8), 10.0, 10.5, 0.05, overviews=(), tiled=False)
    scene["priorities"]["landcover"] = ["worldcover"]
    scene["sources"]["worldcover"] = source("landcover", tmp_path / "src" / "wc.tif", [10.0, 10.0, 10.5, 10.5])
    monkeypatch.setattr(pipeline.landcover, "local_reader", lambda paths: lambda url, r, c: np.full((600, 600), 10, np.uint8))
    monkeypatch.setattr(pipeline, "_run_isolated", lambda fn, *a: fn(*a))
    pkg, _, _ = build_synthetic(tmp_path, scene)
    lc = json.loads((pkg / "landcover" / "index.json").read_text())
    assert lc["codes"] == {"230": "building"}
    mask_index = json.loads((pkg / "features/buildings/mask/index.json").read_text())
    masks = {(t["lat_index"], t["lon_index"]): t["file"] for t in mask_index["tiles"]}
    burned = 0
    for t in lc["tiles"]:
        codes = np.array(Image.open(pkg / "landcover" / t["file"]))
        key = (t["lat_index"], t["lon_index"])
        want = (F.burn_cells(np.array(Image.open(pkg / "features/buildings/mask" / masks[key])))
                if key in masks else np.zeros(codes.shape, bool))
        assert np.array_equal(codes == 230, want)
        burned += int(want.sum())
    assert 20 <= burned <= 50  # ~3,200 m2 at ~9.1 m x 9.3 m cells (lat 10): ~37 cells, fewer at the half-cover edges
