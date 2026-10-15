"""The bridges layer (REALISM R3 bridges) on the synthetic package: its terrain has a valley around lon 10.1356 at
lat 10.160 (hi_dem = 205 + 20 sin(x / 50) cos(y / 70), 0.001 deg pixels), so a 10 km viaduct across it clears the
valley floor by ~7 m."""

import json

import numpy as np
import pytest
from fake_sources import build_synthetic
from shapely.geometry import LineString

from camsim_scene import gltf
from camsim_scene.config import layer_settings, parse_scene
from camsim_scene.features import Feature
from camsim_scene.layers import bridges as layer
from camsim_scene.manifest import STATE_DIR, Manifest, SourceRecord
from camsim_scene.pipeline import build_scene, make_context

LAT = 10.160
VIADUCT = Feature("road", "osm:w1", LineString([(10.09, LAT), (10.18, LAT)]), {"highway": "motorway", "bridge": "yes"})
RAMP_FREE_END = Feature("road", "osm:w2", LineString([(10.13, LAT), (10.13, LAT + 0.01)]),
                        {"highway": "primary", "bridge": "yes"})
LOW = Feature("road", "osm:w3", LineString([(10.30, 10.30), (10.3004, 10.30)]), {"highway": "primary", "bridge": "yes"})
PLAIN = Feature("road", "osm:w4", LineString([(10.2, 10.2), (10.21, 10.2)]), {"highway": "primary"})


def _bridges_settings():
    plan = parse_scene({"name": "t", "bbox": [10.0, 10.0, 10.1, 10.1], "allow": ["ODbL-1.0"],
                        "features": {"roads": True}, "bridges": {}})
    ls = layer_settings(plan)
    return ls["bridges"], ls["landcover"]["features"]


def _with_bridges(pkg, on=True):
    m = Manifest.load(pkg / "manifest.json")
    br, fs = _bridges_settings()
    if not any(s.id == "osm_geofabrik" for s in m.sources):
        m.sources.append(SourceRecord("osm_geofabrik", "osm_geofabrik", "OpenStreetMap", "1", "ODbL-1.0",
                                      "© OpenStreetMap contributors", {}, []))
    m.layers["landcover"]["features"] = fs
    if on:
        m.layers["bridges"] = br
    else:
        m.layers.pop("bridges", None)
    m.write(pkg / "manifest.json")
    return m


@pytest.fixture
def synthetic(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path)
    return pkg, cache


def _build(pkg, cache, monkeypatch, feats):
    from camsim_scene import pipeline

    monkeypatch.setattr(pipeline, "_load_features", lambda m, ctx: list(feats))
    return build_scene(pkg, cache)


def _glbs(pkg):
    root = pkg / layer.OUT
    doc = json.loads((root / "tileset.json").read_text())
    return doc, [(u, gltf.decode_feature_glb((root / u).read_bytes())) for u, _ in layer.walk_tileset(doc)]


def test_layer_writes_tileset_and_decks(synthetic, monkeypatch):
    pkg, cache = synthetic
    _with_bridges(pkg)
    info = _build(pkg, cache, monkeypatch, [VIADUCT, RAMP_FREE_END, LOW, PLAIN])
    doc, tiles = _glbs(pkg)
    ids = sorted(s for _, t in tiles for s in t["columns"]["source_id"])
    assert "osm:w1" in ids and "osm:w3" not in ids and "osm:w4" not in ids
    st = info["bridges"]
    assert st["input"] == 3 and st["dropped_low"] >= 1 and st["kept"] == len(ids)
    root = doc["root"]
    assert root["refine"] == "ADD" and "content" not in root and root["geometricError"] == 100000.0
    for g in root["children"]:
        assert "content" not in g and g["geometricError"] == 700.0
        assert all(t["geometricError"] == 0.0 and t["content"]["uri"].startswith("13/") for t in g["children"])
    hashes = (pkg / "hashes.txt").read_text()
    assert "features/bridges/tileset.json" in hashes and info["layers"]["bridges"]["tiles"] == len(tiles)


def test_deck_ends_are_on_the_terrain_and_the_middle_clears_the_valley(synthetic, monkeypatch):
    pkg, cache = synthetic
    m = _with_bridges(pkg)
    _build(pkg, cache, monkeypatch, [VIADUCT])
    decks, _, hf = layer.plan(m, make_context(pkg, m, cache), [VIADUCT])
    (d,) = decks
    ends = hf(np.array([10.09, 10.18]), np.array([LAT, LAT]))
    assert [d.h0, d.h1] == pytest.approx(list(ends), abs=1e-6)
    assert d.max_clearance_m > 5.0


def test_rebuild_is_identical_and_skips(synthetic, monkeypatch):
    pkg, cache = synthetic
    _with_bridges(pkg)
    _build(pkg, cache, monkeypatch, [VIADUCT])
    before = (pkg / "hashes.txt").read_bytes()
    info = _build(pkg, cache, monkeypatch, [])  # features are not read again when nothing changed
    assert (pkg / "hashes.txt").read_bytes() == before and info["layers"]["bridges"]["skipped"] > 0


def test_no_bridges_layer_is_byte_identical(tmp_path, monkeypatch):
    pkg, cache, _ = build_synthetic(tmp_path)
    before = (pkg / "hashes.txt").read_text()
    info = _build(pkg, cache, monkeypatch, [VIADUCT])
    assert (pkg / "hashes.txt").read_text() == before and "bridges" not in info["layers"]
    assert not (pkg / "features").exists() and not (pkg / STATE_DIR / layer.MARKER).exists()


def test_bridges_off_removes_the_layer(synthetic, monkeypatch):
    pkg, cache = synthetic
    _with_bridges(pkg)
    _build(pkg, cache, monkeypatch, [VIADUCT])
    _with_bridges(pkg, on=False)
    _build(pkg, cache, monkeypatch, [VIADUCT])
    assert not (pkg / "features").exists() and not (pkg / STATE_DIR / layer.MARKER).exists()


def test_buildings_off_keeps_bridges(synthetic, monkeypatch):
    from camsim_scene.layers import buildings as buildings_layer

    pkg, cache = synthetic
    m = _with_bridges(pkg)
    _build(pkg, cache, monkeypatch, [VIADUCT])
    assert "buildings" not in m.layers
    buildings_layer.build(pkg, m, make_context(pkg, m, cache))  # the "no buildings" path
    assert (pkg / layer.OUT / "tileset.json").is_file()


def test_no_bridges_in_box_writes_nothing(synthetic, monkeypatch):
    pkg, cache = synthetic
    _with_bridges(pkg)
    info = _build(pkg, cache, monkeypatch, [PLAIN])
    assert not (pkg / layer.OUT).exists() and info["bridges"]["input"] == 0
    info = _build(pkg, cache, monkeypatch, [PLAIN])
    assert not (pkg / layer.OUT).exists() and info["layers"]["bridges"]["built"] == 0


# --- verify -----------------------------------------------------------------------------------------------------------
import shutil  # noqa: E402
import subprocess  # noqa: E402
from pathlib import Path  # noqa: E402

from camsim_scene.verify import verify  # noqa: E402

VALIDATOR = Path(__file__).parent / "validator"


def _checks(report):
    return {c["name"]: c for c in report["checks"]}


def _verify_deep(pkg, cache, monkeypatch, feats):
    from camsim_scene import pipeline

    monkeypatch.setattr(pipeline, "_load_features", lambda m, ctx: list(feats))
    return _checks(verify(pkg, cache, deep=True))


def test_verify_quick_and_deep_pass(synthetic, monkeypatch):
    pkg, cache = synthetic
    _with_bridges(pkg)
    _build(pkg, cache, monkeypatch, [VIADUCT])
    c = _verify_deep(pkg, cache, monkeypatch, [VIADUCT])
    assert c["bridges_tileset"]["ok"] and c["bridges_heights"]["ok"], c


def test_verify_deep_catches_a_raised_deck(synthetic, monkeypatch):
    pkg, cache = synthetic
    _with_bridges(pkg)
    _build(pkg, cache, monkeypatch, [VIADUCT])
    glb = next((pkg / layer.OUT).rglob("*.glb"))
    t = gltf.decode_feature_glb(glb.read_bytes())
    t["positions"][:, 1] += 0.5
    groups = sorted(t["indices"].items())
    cols = {"kind": ("string", t["columns"]["kind"]), "source_id": ("string", t["columns"]["source_id"]),
            "max_clearance_m": ("float32", np.asarray(t["columns"]["max_clearance_m"], np.float32))}
    from camsim_scene import bridges as BR

    glb.write_bytes(gltf.write_feature_glb(t["positions"], t["normals"], t["feature_ids"], groups, t["materials"],
                                           BR.SCHEMA, "bridge", cols, t["copyright"]))
    c = _verify_deep(pkg, cache, monkeypatch, [VIADUCT])
    assert not c["bridges_heights"]["ok"] and "osm:w1" in c["bridges_heights"]["detail"]


def test_verify_quick_catches_a_missing_tile(synthetic, monkeypatch):
    pkg, cache = synthetic
    _with_bridges(pkg)
    _build(pkg, cache, monkeypatch, [VIADUCT])
    next((pkg / layer.OUT).rglob("*.glb")).unlink()
    c = _checks(verify(pkg))
    assert not c["bridges_tileset"]["ok"] and "missing" in c["bridges_tileset"]["detail"]


@pytest.mark.skipif(shutil.which("npx") is None, reason="needs Node")
def test_3d_tiles_validator_is_clean(synthetic, monkeypatch, tmp_path):
    if not (VALIDATOR / "node_modules").exists():
        subprocess.run(["npm", "ci"], cwd=VALIDATOR, check=True, capture_output=True)
        subprocess.run(["npm", "rebuild", "sharp", "--ignore-scripts=false"], cwd=VALIDATOR, check=True,
                       capture_output=True)
    pkg, cache = synthetic
    _with_bridges(pkg)
    _build(pkg, cache, monkeypatch, [VIADUCT, RAMP_FREE_END])
    out = tmp_path / "report.json"
    subprocess.run(["npx", "3d-tiles-validator", "--tilesetFile", str(pkg / layer.OUT / "tileset.json"),
                    "--reportFile", str(out)], cwd=VALIDATOR, check=False, capture_output=True)
    report = json.loads(out.read_text())
    assert report["numErrors"] == 0, json.dumps(report["issues"], indent=1)[:4000]
