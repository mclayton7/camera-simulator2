import json
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pytest
from fake_sources import build_synthetic, source
from PIL import Image
from rasters import write_geotiff
from test_buildings_layer import ROWS, _scene

from camsim_scene import gltf, pipeline
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import build_scene
from camsim_scene.verify import verify

VALIDATOR = Path(__file__).parent / "validator"


def _checks(report):
    return {c["name"]: c for c in report["checks"]}


def test_quick_and_deep_pass(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    c = _checks(verify(pkg, cache, deep=True))
    assert c["buildings_tileset"]["ok"] and c["buildings_heights"]["ok"], c


def test_deep_catches_an_edited_height(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    glb = next((pkg / "features/buildings").rglob("*.glb"))
    t = gltf.decode_tile(glb.read_bytes())
    t.positions[:, 1] += 1.0  # raise every vertex 1 m
    glb.write_bytes(gltf.write_glb(gltf.TileMesh(t.positions, t.normals, t.feature_ids, t.roof_indices,
                                                 t.wall_indices), t.table, t.copyright))
    c = _checks(verify(pkg, cache, deep=True))
    assert not c["buildings_heights"]["ok"] and not c["hashes"]["ok"]


def test_quick_catches_a_missing_tile(tmp_path):
    pkg, _, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    next((pkg / "features/buildings").rglob("*.glb")).unlink()
    c = _checks(verify(pkg))
    assert not c["buildings_tileset"]["ok"] and "missing" in c["buildings_tileset"]["detail"]


@pytest.mark.skipif(shutil.which("npx") is None, reason="needs Node")
@pytest.mark.parametrize("textured", [False, True])
def test_3d_tiles_validator_is_clean(tmp_path, textured):
    if not (VALIDATOR / "node_modules").exists():
        subprocess.run(["npm", "ci"], cwd=VALIDATOR, check=True, capture_output=True)
        # .npmrc skips install scripts (better-sqlite3 does not compile on current Node); sharp needs its binary
        subprocess.run(["npm", "rebuild", "sharp", "--ignore-scripts=false"], cwd=VALIDATOR, check=True,
                       capture_output=True)
    from test_buildings_layer import _roof_scene

    pkg, _, _ = build_synthetic(tmp_path, _roof_scene(tmp_path) if textured else _scene(tmp_path, ROWS))
    out = tmp_path / "report.json"
    subprocess.run(["npx", "3d-tiles-validator", "--tilesetFile", str(pkg / "features/buildings/tileset.json"),
                    "--reportFile", str(out)], cwd=VALIDATOR, check=False, capture_output=True)
    report = json.loads(out.read_text())
    assert report["numErrors"] == 0, json.dumps(report["issues"], indent=1)[:4000]


def _lc_roof_package(tmp_path, monkeypatch, name="pkg"):
    """The textured-roof scene with land cover (tree cover everywhere through a patched COG reader, cut in-process)."""
    from test_buildings_layer import _roof_scene

    scene = _roof_scene(tmp_path)
    write_geotiff(tmp_path / "src" / "wc.tif", np.full((10, 10), 10, np.uint8), 10.0, 10.5, 0.05,
                  overviews=(), tiled=False)
    scene["priorities"]["landcover"] = ["worldcover"]
    scene["sources"]["worldcover"] = source("landcover", tmp_path / "src" / "wc.tif", [10.0, 10.0, 10.5, 10.5])
    monkeypatch.setattr(pipeline.landcover, "local_reader",
                        lambda paths: lambda url, r, c: np.full((600, 600), 10, np.uint8))
    monkeypatch.setattr(pipeline, "_run_isolated", lambda fn, *a: fn(*a))
    return build_synthetic(tmp_path, scene, name=name)


def _burned_tile(pkg):
    lc = pkg / "landcover"
    for t in json.loads((lc / "index.json").read_text())["tiles"]:
        codes = np.array(Image.open(lc / t["file"]))
        if (codes == 230).any():
            return lc / t["file"], codes
    raise AssertionError("no burned land-cover tile")


def test_deep_checks_mask_burn_and_roofs(tmp_path, monkeypatch):
    pkg, cache, _ = _lc_roof_package(tmp_path, monkeypatch)
    c = _checks(verify(pkg, cache, deep=True))
    assert c["buildings_mask"]["ok"] and c["buildings_roofs"]["ok"] and c["buildings_tileset"]["ok"], c


def test_deep_catches_a_cell_the_burn_lost_or_invented(tmp_path, monkeypatch):
    pkg, cache, _ = _lc_roof_package(tmp_path, monkeypatch)
    path, codes = _burned_tile(pkg)
    r, col = np.argwhere(codes == 230)[0]
    codes[r, col] = 10  # a cell >= 32/64 covered that is no longer a building
    codes[0, 0] = 230  # a building cell with no mask under it
    Image.fromarray(codes).save(path, format="PNG", optimize=True)
    c = _checks(verify(pkg, cache, deep=True))
    assert not c["buildings_mask"]["ok"]
    assert "not 230" in c["buildings_mask"]["detail"] and "without" in c["buildings_mask"]["detail"]


def test_deep_catches_a_missing_roof_texel(tmp_path, monkeypatch):
    pkg, cache, _ = _lc_roof_package(tmp_path, monkeypatch)
    glb = next((pkg / "features/buildings").rglob("*.glb"))
    t = gltf.decode_tile(glb.read_bytes())
    glb.write_bytes(gltf.write_glb(gltf.TileMesh(t.positions, t.normals, t.feature_ids, t.roof_indices,
                                                 t.wall_indices, t.textured_indices, t.texcoords, t.texture, None),
                                   t.table, t.copyright))
    c = _checks(verify(pkg, cache, deep=True))
    assert not c["buildings_roofs"]["ok"] and "roof_texel_m" in c["buildings_roofs"]["detail"]


def test_quick_catches_a_missing_mask_tile(tmp_path):
    pkg, _, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    next((pkg / "features/buildings/mask").glob("*.png")).unlink()
    c = _checks(verify(pkg))
    assert not c["buildings_tileset"]["ok"] and "missing mask/" in c["buildings_tileset"]["detail"]


def test_rebuild_with_textures_and_burn_is_identical(tmp_path, monkeypatch):
    a, cache, _ = _lc_roof_package(tmp_path, monkeypatch, name="a")
    b = tmp_path / "b"
    b.mkdir()
    (b / "manifest.json").write_bytes((a / "manifest.json").read_bytes())
    build_scene(b, cache, jobs=1)
    assert (a / "hashes.txt").read_bytes() == (b / "hashes.txt").read_bytes()


def test_roof_texel_change_rebuilds_tiles_not_the_mask(tmp_path, monkeypatch):
    pkg, cache, _ = _lc_roof_package(tmp_path, monkeypatch)
    before = dict(line.split("  ", 1)[::-1] for line in (pkg / "hashes.txt").read_text().splitlines())
    m = Manifest.load(pkg / "manifest.json")
    m.layers["buildings"]["roof_texel_m"] = 1.0
    m.write(pkg / "manifest.json")
    info = build_scene(pkg, cache, jobs=1)
    after = dict(line.split("  ", 1)[::-1] for line in (pkg / "hashes.txt").read_text().splitlines())
    glbs = [k for k in before if k.endswith(".glb")]
    burn = [k for k in before if k.startswith(("features/buildings/mask/", "landcover/"))]
    assert glbs and burn and set(before) == set(after)
    assert all(before[k] != after[k] for k in glbs)  # every tile here has a textured roof
    assert all(before[k] == after[k] for k in burn)  # mask and land cover byte-identical
    assert info["buildings"]["roofs"]["tiles_by_texel_m"] == {"1": len(glbs)}
