import json
import shutil
import subprocess
from pathlib import Path

import pytest
from fake_sources import build_synthetic
from test_buildings_layer import ROWS, _scene

from camsim_scene import gltf
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
