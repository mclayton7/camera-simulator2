import json

from fake_sources import build_synthetic, synthetic_scene
from overture_fixture import box_lonlat, write_overture_parquet

from camsim_scene import buildings as B
from camsim_scene import gltf
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import build_scene


def _scene(tmp_path, rows):
    scene = synthetic_scene(tmp_path / "src")
    pq = write_overture_parquet(tmp_path / "src" / "overture.parquet", rows)
    scene["allow"] = ["ODbL-1.0"]
    scene["buildings"] = {}
    scene.setdefault("sources", {})["overture_buildings"] = {"parquet": str(pq)}
    return scene


ROWS = [
    {"id": "b1", "geom": box_lonlat(10.2, 10.2, 60, 50), "height": 12.0, "class": "industrial"},
    {"id": "b2", "geom": box_lonlat(10.21, 10.2, 12, 10), "num_floors": 2},
    {"id": "b3", "geom": box_lonlat(10.22, 10.21, 8, 8)},
]


def test_layer_writes_tileset_and_tiles(tmp_path):
    pkg, _, info = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    doc = json.loads((pkg / "features/buildings/tileset.json").read_text())
    uris = [u for u, _ in B.walk_tileset(doc)]
    assert uris and all((pkg / "features/buildings" / u).is_file() for u in uris)
    ids = sorted(s for u in uris for s in gltf.decode_tile((pkg / "features/buildings" / u).read_bytes()).table.source_ids)
    assert ids == ["b1", "b2", "b3"]
    assert info["layers"]["buildings"]["tiles"] == len(uris) and info["buildings"]["kept"] == 3
    hashes = (pkg / "hashes.txt").read_text()
    assert "features/buildings/tileset.json" in hashes
    m = Manifest.load(pkg / "manifest.json")
    assert m.source("overture_buildings").assets[0].sha256


def test_rebuild_is_identical_and_skips(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    before = (pkg / "hashes.txt").read_bytes()
    info = build_scene(pkg, cache, jobs=1)
    assert info["layers"]["buildings"]["skipped"] == info["layers"]["buildings"]["tiles"]
    assert (pkg / "hashes.txt").read_bytes() == before


def test_rebuild_from_manifest_after_release_is_gone(tmp_path):
    scene = _scene(tmp_path, ROWS)
    a, cache, _ = build_synthetic(tmp_path, scene, name="a")
    (tmp_path / "src" / "overture.parquet").unlink()
    b = tmp_path / "b"
    b.mkdir()
    (b / "manifest.json").write_bytes((a / "manifest.json").read_bytes())
    build_scene(b, cache, jobs=1)
    assert (a / "hashes.txt").read_bytes() == (b / "hashes.txt").read_bytes()


def test_no_buildings_section_has_no_features_dir(tmp_path):
    pkg, _, info = build_synthetic(tmp_path)
    assert not (pkg / "features").exists() and "buildings" not in info["layers"]


def test_zero_kept_buildings_writes_no_tileset_and_skips(tmp_path):
    rows = [{"id": "tiny", "geom": box_lonlat(10.2, 10.2, 1, 1)}]
    pkg, cache, info = build_synthetic(tmp_path, _scene(tmp_path, rows))
    assert not (pkg / "features").exists()
    assert info["layers"]["buildings"]["tiles"] == 0 and info["buildings"]["kept"] == 0
    again = build_scene(pkg, cache, jobs=1)
    assert again["layers"]["buildings"]["tiles"] == 0 and again["buildings"]["kept"] == 0


def test_terrain_setting_change_forces_rebuild(tmp_path):
    from camsim_scene.layers import buildings as layer
    from camsim_scene.pipeline import make_context

    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, ROWS))
    m = Manifest.load(pkg / "manifest.json")
    assert layer.build(pkg, m, make_context(pkg, m, cache)).skipped > 0
    m.layers["terrain"]["probe"] = 1
    st = layer.build(pkg, m, make_context(pkg, m, cache))
    assert st.built > 0 and st.skipped == 0
