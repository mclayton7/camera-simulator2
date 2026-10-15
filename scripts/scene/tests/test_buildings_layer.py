import io
import json

import numpy as np
from fake_sources import build_synthetic, fine_roof_scene, fine_truth, synthetic_scene
from overture_fixture import box_lonlat, write_overture_parquet
from PIL import Image

from camsim_scene import buildings as B
from camsim_scene import geodesy, gltf
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


def _roof_scene(tmp_path, rows=ROWS, **buildings):
    scene = fine_roof_scene(tmp_path / "src")
    pq = write_overture_parquet(tmp_path / "src" / "overture.parquet", rows)
    scene["allow"] = ["ODbL-1.0"]
    scene["buildings"] = dict(buildings)
    scene.setdefault("sources", {})["overture_buildings"] = {"parquet": str(pq)}
    return scene


def test_roof_atlas_matches_the_imagery(tmp_path):
    pkg, _, info = build_synthetic(tmp_path, _roof_scene(tmp_path))
    root = pkg / "features/buildings"
    checked = 0
    for uri, mat in B.walk_tileset(json.loads((root / "tileset.json").read_text())):
        t = gltf.decode_tile((root / uri).read_bytes())
        assert t.extras["roof_texel_m"] == 0.5 and t.texture is not None
        atlas = np.asarray(Image.open(io.BytesIO(t.texture)).convert("RGB"))
        size = atlas.shape[0]
        assert atlas.shape[1] == size and size & (size - 1) == 0 and size <= 2048
        tri = t.textured_indices.reshape(-1, 3)
        uv = t.texcoords[tri].astype(np.float64).mean(axis=1)
        p = t.positions[tri].astype(np.float64).mean(axis=1)
        enu = np.stack([p[:, 0], -p[:, 2], p[:, 1]], axis=1)
        lon, lat, _ = geodesy.ecef_to_geodetic(enu @ mat[:3, :3].T + mat[:3, 3])
        got = atlas[np.floor(uv[:, 1] * size).astype(int), np.floor(uv[:, 0] * size).astype(int)].astype(int)
        assert np.abs(got - fine_truth(lon, lat).T.astype(int)).max() <= 8  # JPEG q85 + half a texel of ramp
        checked += len(tri)
    assert checked > 0
    assert info["buildings"]["roofs"]["textured"] == 3 and info["buildings"]["roofs"]["flat_coarse"] == 0


def test_roof_textures_off_writes_flat_r3_tiles(tmp_path):
    pkg, _, info = build_synthetic(tmp_path, _roof_scene(tmp_path, roof_textures=False))
    for glb in (pkg / "features/buildings").rglob("*.glb"):
        doc, _ = gltf.read_glb(glb.read_bytes())
        assert "images" not in doc and "extras" not in doc and len(doc["materials"]) == 2
    assert "roofs" not in info["buildings"]


def test_coarse_imagery_keeps_every_roof_flat(tmp_path):
    pkg, _, info = build_synthetic(tmp_path, _scene(tmp_path, ROWS))  # 111 m imagery only
    for glb in (pkg / "features/buildings").rglob("*.glb"):
        t = gltf.decode_tile(glb.read_bytes())
        assert t.texture is None and t.extras == {"roof_texel_m": None}
    assert info["buildings"]["roofs"]["flat_coarse"] == 3 and info["buildings"]["roofs"]["textured"] == 0


from test_buildings_chunks import _edge_lon  # noqa: E402

EDGE_ROWS = [
    *ROWS,
    {"id": "e1", "geom": box_lonlat(_edge_lon() - 3e-5, 10.2, 30, 20), "height": 7.0},
    {"id": "e2", "geom": box_lonlat(_edge_lon() + 3e-5, 10.2, 30, 20)},
    {"id": "e3", "geom": box_lonlat(10.2499, 10.2, 40, 40)},  # straddles the 10.25 land-cover tile edge
]


def _whole_box(pkg, cache):
    """Reference: the pre-chunk path (one plan of the whole snapshot)."""
    from camsim_scene import footprints as F
    from camsim_scene.context import WorkerState
    from camsim_scene.layers import buildings as layer
    from camsim_scene.pipeline import make_context

    m = Manifest.load(pkg / "manifest.json")
    ctx = make_context(pkg, m, cache)
    st = WorkerState(ctx)
    rec = m.source(m.layers["buildings"]["priorities"][0])
    blds, stats = B.plan_buildings(B.read_snapshot(ctx.asset_paths[f"{rec.id}/{rec.assets[0].id}"]),
                                   layer.heights_fn(st, m), float(m.layers["buildings"]["min_area_m2"]))
    return blds, stats, F.raster_tiles(blds)


def test_chunked_plan_and_raster_equal_the_whole_box(tmp_path):
    from camsim_scene import footprints as F
    from camsim_scene.layers import buildings as layer
    from camsim_scene.manifest import STATE_DIR

    pkg, cache, info = build_synthetic(tmp_path, _scene(tmp_path, EDGE_ROWS))
    blds, stats, raster = _whole_box(pkg, cache)
    chunks = json.loads((pkg / STATE_DIR / "buildings" / "partition" / "partition.json").read_text())["chunks"]
    assert len(chunks) >= 2
    got = sorted((b for c in chunks for b in layer.load_chunk_plan(pkg, *c)), key=lambda b: b.source_id)

    def rec(b):
        return b.source_id, b.tile, b.base_m, b.roof_m, [p.wkb for p in b.parts]

    assert [rec(b) for b in got] == [rec(b) for b in blds]
    assert {k: v for k, v in info["buildings"].items() if k != "roofs"} == stats
    fr = F.FootprintRaster(pkg / STATE_DIR / "buildings-footprints")
    assert fr.keys() == list(raster) and all(np.array_equal(fr.get(*k), raster[k]) for k in raster)


def test_partition_without_marker_is_redone(tmp_path):
    from camsim_scene.manifest import STATE_DIR

    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, EDGE_ROWS))
    before = (pkg / "hashes.txt").read_bytes()
    (pkg / STATE_DIR / "buildings" / "partition" / "partition.json").unlink()
    (pkg / STATE_DIR / "buildings-footprints" / "index.json").unlink()
    (pkg / STATE_DIR / "landcover.json").unlink(missing_ok=True)  # force the land-cover cut, when it burns
    build_scene(pkg, cache, jobs=1)
    assert (pkg / "hashes.txt").read_bytes() == before


def test_stale_chunk_plan_is_replanned_alone(tmp_path, monkeypatch):
    from camsim_scene.manifest import STATE_DIR

    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, EDGE_ROWS))
    plan_dir = pkg / STATE_DIR / "buildings" / "plan"
    victim = sorted(plan_dir.glob("*.json"))[0]
    victim.write_text(json.dumps({"inputs": "stale"}))
    (pkg / STATE_DIR / "buildings-footprints" / "index.json").unlink()
    (pkg / STATE_DIR / "buildings.json").unlink()  # the layer rebuilds (its marker is gone) and needs every chunk
    calls = []
    real = B.plan_buildings
    monkeypatch.setattr(B, "plan_buildings", lambda fps, *a, **k: (calls.append(1), real(fps, *a, **k))[1])
    before = (pkg / "hashes.txt").read_bytes()
    build_scene(pkg, cache, jobs=1)
    assert len(calls) == 1 and (pkg / "hashes.txt").read_bytes() == before


def test_chunked_glbs_and_tileset_equal_the_whole_box(tmp_path):
    from camsim_scene.manifest import canonical_json

    pkg, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, EDGE_ROWS))
    blds, _, _ = _whole_box(pkg, cache)
    by = {}
    for b in blds:
        by.setdefault(b.tile, []).append(b)
    tiles = {k: B.tile_summary(k, v) for k, v in sorted(by.items())}
    assert (pkg / "features/buildings/tileset.json").read_text() == canonical_json(B.tileset_json(tiles))
    for (z, x, y), bl in by.items():
        mesh, table = B.tile_mesh(bl, tiles[(z, x, y)][1])
        got = gltf.decode_tile((pkg / "features/buildings" / f"{z}/{x}/{y}.glb").read_bytes())
        assert got.table.source_ids == table.source_ids and np.array_equal(got.positions, mesh.positions)


def test_near_only_from_detail_margins():
    from types import SimpleNamespace

    from camsim_scene.layers import buildings as layer

    box = [[10, 10], [11, 10], [11, 11], [10, 11], [10, 10]]
    small = [[10, 10], [10.1, 10], [10.1, 10.1], [10, 10.1], [10, 10]]
    assert layer.near_only_fn(SimpleNamespace(regions=[{"name": "bbox", "polygon": box}])) is None
    fn = layer.near_only_fn(SimpleNamespace(regions=[{"name": "bbox", "polygon": box},
                                                     {"name": "detail_margin:a", "polygon": small}]))
    inside = [np.radians(v) for v in (10.05, 10.05, 10.06, 10.06)] + [0.0, 10.0]
    outside = [np.radians(v) for v in (10.5, 10.5, 10.6, 10.6)] + [0.0, 10.0]
    assert fn(inside) is False and fn(outside) is True


def test_build_in_the_pool_equals_inline(tmp_path):
    a, cache, _ = build_synthetic(tmp_path, _scene(tmp_path, EDGE_ROWS), name="a")
    b = tmp_path / "b"
    b.mkdir()
    (b / "manifest.json").write_bytes((a / "manifest.json").read_bytes())
    info = build_scene(b, cache, jobs=2)
    assert (a / "hashes.txt").read_bytes() == (b / "hashes.txt").read_bytes()
    assert info["layers"]["buildings"]["peak_rss_mb"] > 0
