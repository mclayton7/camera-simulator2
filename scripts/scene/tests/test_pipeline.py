import io
import json

import numpy as np
import pytest
import shapely
from fake_sources import build_synthetic, source, synthetic_scene
from fakes import FakeHttp
from rasters import write_geotiff

from camsim_scene import fsutil, qmesh, tiling
from camsim_scene.config import parse_scene
from camsim_scene.engine import BuildLock, BuildLocked
from camsim_scene.layers import terrain
from camsim_scene.licences import LicenceError
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import BuildError, PlanError, build_scene, estimate, plan_scene


def test_end_to_end_layout_and_hashes(tmp_path):
    pkg, _, info = build_synthetic(tmp_path)
    lj = json.loads((pkg / "terrain/layer.json").read_text())
    assert lj["available"][0] == [{"startX": 0, "startY": 0, "endX": 1, "endY": 0}]
    assert lj["available"][2] == [{"startX": 0, "startY": 0, "endX": 7, "endY": 3}] and lj["maxzoom"] == 6
    assert '<TileSet href="6"' in (pkg / "imagery/tilemapresource.xml").read_text()
    m = Manifest.load(pkg / "manifest.json")
    assert m.hashes_sha256 == fsutil.sha256_file(pkg / "hashes.txt")
    t = info["layers"]["terrain"]
    assert t["tiles"] == t["built"] == len(list((pkg / "terrain").rglob("*.terrain"))) > 0
    assert info["layers"]["imagery"]["built"] == len(list((pkg / "imagery").rglob("*.jpg")))
    for p in (pkg / "terrain").rglob("*.terrain"):
        assert np.isfinite(qmesh.decode(p.read_bytes()).heights()).all()
    assert "synthetic test data" in (pkg / "ATTRIBUTION.txt").read_text()
    assert all(a.metadata["footprint"] for a in m.source("hi_dem").assets)


def test_rebuild_skips_everything(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path)
    before = (pkg / "hashes.txt").read_bytes()
    info = build_scene(pkg, cache, jobs=1)
    assert info["layers"]["terrain"]["built"] == 0 and info["layers"]["imagery"]["built"] == 0
    assert (pkg / "hashes.txt").read_bytes() == before


def test_worker_count_does_not_change_bytes(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    a, cache, _ = build_synthetic(tmp_path, scene, jobs=1, name="a")
    b, _, _ = build_synthetic(tmp_path, scene, jobs=2, name="b", cache=cache)
    assert (a / "hashes.txt").read_bytes() == (b / "hashes.txt").read_bytes()
    assert (a / "manifest.json").read_bytes() == (b / "manifest.json").read_bytes()


def test_rebuild_from_the_manifest_alone_is_identical(tmp_path):
    a, cache, _ = build_synthetic(tmp_path, name="a")
    b = tmp_path / "b"
    b.mkdir()
    (b / "manifest.json").write_bytes((a / "manifest.json").read_bytes())
    build_scene(b, cache, jobs=1)
    assert (a / "hashes.txt").read_bytes() == (b / "hashes.txt").read_bytes()


def test_imagery_setting_dirties_only_imagery(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path)
    m = Manifest.load(pkg / "manifest.json")
    m.layers["imagery"]["jpeg_quality"] = 70
    m.write(pkg / "manifest.json")
    info = build_scene(pkg, cache, jobs=1)
    assert info["layers"]["terrain"]["built"] == 0
    assert info["layers"]["imagery"]["built"] == info["layers"]["imagery"]["tiles"]


def test_new_asset_dirties_only_its_tiles(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    pkg, cache, _ = build_synthetic(tmp_path, scene)
    patch_bbox = [10.40, 10.40, 10.45, 10.45]
    write_geotiff(
        tmp_path / "src/patch.tif",
        np.full((50, 50), 300.0, np.float32),
        10.40,
        10.45,
        0.001,
        nodata=-9999.0,
        overviews=(),
    )
    scene["sources"]["hi_dem"]["files"].append(
        {"id": "patch", "path": str(tmp_path / "src/patch.tif"), "bbox": patch_bbox}
    )
    plan_scene(parse_scene(scene), pkg, http=FakeHttp({}), out=io.StringIO())
    info = build_scene(pkg, cache, jobs=1)
    keys = tiling.available_keys(json.loads((pkg / "terrain/layer.json").read_text())["available"])
    patch = shapely.box(*patch_bbox)
    expected = sum(
        shapely.box(*terrain.query_bounds(z, int(k) >> 32, int(k) & 0xFFFFFFFF)).intersects(patch)
        for z, ks in keys.items()
        for k in ks
    )
    assert 0 < info["layers"]["terrain"]["built"] == expected < info["layers"]["terrain"]["tiles"]
    assert info["layers"]["imagery"]["built"] == 0


def test_build_removes_stale_tiles(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path)
    (pkg / "terrain/9/0").mkdir(parents=True)
    (pkg / "terrain/9/0/0.terrain").write_bytes(b"old")
    (pkg / "imagery/6/0").mkdir(parents=True, exist_ok=True)
    (pkg / "imagery/6/0/0.jpg").write_bytes(b"old")
    build_scene(pkg, cache, jobs=1)
    assert not (pkg / "terrain/9").exists() and not (pkg / "imagery/6/0/0.jpg").exists()
    assert "terrain/9/0/0.terrain" not in (pkg / "hashes.txt").read_text()


def test_build_refuses_when_locked(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path)
    with BuildLock(pkg), pytest.raises(BuildLocked):
        build_scene(pkg, cache, jobs=1)


def test_all_nodata_asset_adds_no_tiles(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    _, _, info0 = build_synthetic(tmp_path, scene, name="base")
    write_geotiff(tmp_path / "src/empty.tif", np.zeros((3, 100, 100), np.uint8), 10.0, 10.5, 0.005, overviews=())
    scene["sources"]["empty_rgb"] = source(
        "imagery",
        tmp_path / "src/empty.tif",
        [10.0, 10.0, 10.5, 10.5],
        max_zoom=12,
        bands=[1, 2, 3],
        nodata_rule="all_zero",
    )
    scene["priorities"]["imagery"] = ["empty_rgb", *scene["priorities"]["imagery"]]
    pkg, _, info = build_synthetic(tmp_path, scene, name="with_empty")
    assert info["layers"]["imagery"]["tiles"] == info0["layers"]["imagery"]["tiles"]
    assert Manifest.load(pkg / "manifest.json").source("empty_rgb").assets[0].metadata["footprint"] is None


def test_licence_outside_the_allow_list_fails_plan(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    scene["sources"]["hi_rgb"]["licence"] = "ODbL-1.0"
    with pytest.raises(LicenceError, match="--allow ODbL-1.0"):
        plan_scene(parse_scene(scene), tmp_path / "pkg", http=FakeHttp({}), out=io.StringIO())
    scene["allow"] = ["ODbL-1.0"]
    m = plan_scene(parse_scene(scene), tmp_path / "pkg", http=FakeHttp({}), out=io.StringIO())
    assert "ODbL-1.0" in m.licence_allow


def test_source_under_the_wrong_layer_fails_plan(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    scene["priorities"]["terrain"] = ["hi_rgb", "base_dem"]
    with pytest.raises(PlanError, match="hi_rgb"):
        plan_scene(parse_scene(scene), tmp_path / "pkg", http=FakeHttp({}), out=io.StringIO())


def test_estimate_before_fetch(tmp_path):
    out = io.StringIO()
    m = plan_scene(parse_scene(synthetic_scene(tmp_path / "src")), tmp_path / "pkg", http=FakeHttp({}), out=out)
    est = estimate(m)
    assert est["tiles"]["terrain"] > 0 and est["package_bytes"] > 0 and "tiles" in out.getvalue()


def test_rss_bound_is_reported_after_a_complete_build(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    from camsim_scene.cache import Cache

    pkg = tmp_path / "pkg"
    plan_scene(parse_scene(scene), pkg, http=FakeHttp({}), out=io.StringIO())
    with pytest.raises(BuildError, match="RSS"):
        build_scene(pkg, Cache(tmp_path / "cache"), jobs=1, max_worker_rss_mb=1.0)
    assert (pkg / "hashes.txt").exists() and (pkg / "build.json").exists()


def test_build_refuses_when_locked_before_fetching(tmp_path):
    from camsim_scene.cache import Cache

    pkg = tmp_path / "pkg"
    plan_scene(parse_scene(synthetic_scene(tmp_path / "src")), pkg, http=FakeHttp({}), out=io.StringIO())
    before = (pkg / "manifest.json").read_bytes()
    with BuildLock(pkg), pytest.raises(BuildLocked):
        build_scene(pkg, Cache(tmp_path / "cache"), jobs=1)
    assert (pkg / "manifest.json").read_bytes() == before
    assert not (tmp_path / "cache").exists()


def test_landcover_skip_requires_every_listed_tile(tmp_path):
    from camsim_scene.pipeline import _landcover_complete

    (tmp_path / "a.png").write_bytes(b"png")
    assert _landcover_complete({"tiles": [{"file": "a.png"}]}, tmp_path)
    assert not _landcover_complete({"tiles": [{"file": "a.png"}, {"file": "b.png"}]}, tmp_path)


def test_build_recomputes_missing_footprints_of_a_hashed_manifest(tmp_path):
    pkg, cache, _ = build_synthetic(tmp_path)
    m = Manifest.load(pkg / "manifest.json")
    for a in m.source("hi_dem").assets + m.source("hi_rgb").assets:
        del a.metadata["footprint"]
    m.write(pkg / "manifest.json")
    assert m.is_fetched()
    info = build_scene(pkg, cache, jobs=1)
    assert info["layers"]["terrain"]["built"] == 0
    assert all(a.metadata["footprint"] for a in Manifest.load(pkg / "manifest.json").source("hi_dem").assets)


def test_missing_footprint_outside_a_build_is_a_clear_error(tmp_path):
    from camsim_scene.context import ContextError, coverage

    pkg, _, _ = build_synthetic(tmp_path)
    m = Manifest.load(pkg / "manifest.json")
    del m.source("hi_dem").assets[0].metadata["footprint"]
    with pytest.raises(ContextError, match="camsim-scene fetch"):
        coverage(m, "terrain")
