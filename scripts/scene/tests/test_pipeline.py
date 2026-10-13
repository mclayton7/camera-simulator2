import io
import json
import os

import numpy as np
import pytest
import shapely
from PIL import Image
from shapely.geometry import LineString
from fake_sources import build_synthetic, source, synthetic_scene
from fakes import FakeHttp, coops_http
from rasters import write_geotiff

from camsim_scene import fsutil, pipeline, qmesh, tiling
from camsim_scene.config import parse_scene
from camsim_scene.context import coverage
from camsim_scene.engine import BuildLock, BuildLocked
from camsim_scene.features import Feature
from camsim_scene.layers import terrain
from camsim_scene.licences import LicenceError
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import BuildError, PlanError, build_scene, estimate, plan_scene
from camsim_scene.tiling import plan_tiles


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


def test_estimate_counts_ndvi_tiles(tmp_path):
    from fake_sources import ndvi_scene

    out = io.StringIO()
    m = plan_scene(parse_scene(ndvi_scene(tmp_path / "src")), tmp_path / "pkg", http=FakeHttp({}), out=out)
    est = estimate(m)
    assert 0 < est["tiles"]["ndvi"] <= est["tiles"]["imagery"]
    assert f"ndvi {est['tiles']['ndvi']}" in out.getvalue()
    base = sum(est["tiles"][k] * pipeline.EST_KB[k] * 1024 for k in ("terrain", "imagery", "landcover"))
    assert est["package_bytes"] == int(base + est["tiles"]["ndvi"] * pipeline.EST_KB["ndvi"] * 1024)


def test_ndvi_parent_errors_on_a_planned_child_that_was_not_built():
    def read(layer, z, x, y):
        return None if (x, y) == (3, 1) else {"output": f"h{x}{y}"}

    with pytest.raises(BuildError, match="not built"):
        pipeline._ndvi_child_outputs(read, 4, 1, 0, (True, True, True, True))
    outs = pipeline._ndvi_child_outputs(read, 4, 1, 0, (True, True, True, False))  # child 3/1 unplanned: "absent"
    assert outs[3] == "absent" and outs[:3] == ["h20", "h30", "h21"]


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


def test_build_json_counts_files_per_layer(tmp_path, monkeypatch):
    from camsim_scene import pipeline
    from camsim_scene.context import BuildContext
    from camsim_scene.manifest import SourceRecord

    pkg, _, info = build_synthetic(tmp_path)
    for layer in ("terrain", "imagery"):
        assert info["layers"][layer]["files"] == info["layers"][layer]["tiles"] > 0
    assert info["layers"]["landcover"]["files"] == 0
    assert json.loads((pkg / "build.json").read_text())["layers"]["terrain"]["files"] > 0
    m = Manifest.load(pkg / "manifest.json")
    m.sources.append(SourceRecord("lc", "worldcover", "lc", "1", "CC-BY-4.0", "lc", {}, []))
    m.layers["landcover"] = {"priorities": ["lc"], "bounds": [10.0, 10.0, 10.1, 10.04], "tile_deg": 0.05}
    monkeypatch.setattr(
        pipeline.landcover, "local_reader", lambda paths: lambda url, r, c: np.zeros((600, 600), np.uint8)
    )
    monkeypatch.setattr(pipeline, "_run_isolated", lambda fn, *a: fn(*a))  # the patched reader stays in-process
    st = pipeline._build_landcover(pkg, m, BuildContext(str(pkg), m.to_dict(), {}, {}))
    assert st.tiles == 2 and st.files == 4  # two PNGs + index.json + ATTRIBUTION.txt
    st = pipeline._build_landcover(pkg, m, BuildContext(str(pkg), m.to_dict(), {}, {}))
    assert st.skipped == 2 and st.files == 4


def test_run_isolated_uses_another_process():
    import os

    from camsim_scene import pipeline

    assert pipeline._run_isolated(os.getpid) != os.getpid()


def _planned(tmp_path, margin_km, name):
    scene = synthetic_scene(tmp_path / "src")
    scene["imagery_margin_km"] = margin_km
    return plan_scene(parse_scene(scene), tmp_path / name, http=FakeHttp({}), out=io.StringIO())


def test_margin_changes_imagery_tiles_not_terrain(tmp_path):
    m0, m3 = _planned(tmp_path, 0, "p0"), _planned(tmp_path, 3, "p3")
    t0, t3 = estimate(m0)["tiles"], estimate(m3)["tiles"]
    assert t0["terrain"] == t3["terrain"]
    p0 = plan_tiles(m0.region_objs(), "terrain", coverage(m0, "terrain", use_bbox=True)).tiles
    p3 = plan_tiles(m3.region_objs(), "terrain", coverage(m3, "terrain", use_bbox=True)).tiles
    assert p0.keys() == p3.keys() and all(np.array_equal(p0[z], p3[z]) for z in p0)
    assert m3.layers["imagery"]["margin_km"] == 3.0 and t3["imagery"] >= t0["imagery"]


def test_naip_discovery_receives_the_margin_area(tmp_path, monkeypatch):
    from camsim_scene.sources.naip_pc import NaipPc

    seen = []
    monkeypatch.setattr(NaipPc, "discover", lambda self, area: seen.append(tuple(area.bounds)) or [])
    scene = synthetic_scene(tmp_path / "src")
    scene["imagery_margin_km"] = 3
    scene["priorities"]["imagery"] = ["naip_pc", *scene["priorities"]["imagery"]]
    plan = parse_scene(scene)
    plan_scene(plan, tmp_path / "pkg", http=FakeHttp({}), out=io.StringIO())
    assert seen == [plan.area("margin")] and seen[0] != plan.bbox


def _crm_scene(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    scene["priorities"]["terrain"].insert(1, "crm")
    scene["sources"]["crm"] = source(
        "terrain", tmp_path / "src" / "base_dem.tif", [-180, -90, 180, 90], adapter="fake_sources:FakeMslSource"
    )
    return scene


def test_crm_assets_without_station_is_a_plan_error(tmp_path):
    with pytest.raises(PlanError, match=r"\[sea_level\] station"):
        plan_scene(parse_scene(_crm_scene(tmp_path)), tmp_path / "pkg", http=FakeHttp({}))


def test_station_metadata_reaches_crm_assets(tmp_path):
    scene = _crm_scene(tmp_path)
    scene["sea_level"] = {"station": "9410230"}
    m = plan_scene(parse_scene(scene), tmp_path / "pkg", http=coops_http(), out=io.StringIO())
    assert m.sea_level["msl_above_navd88_m"] == pytest.approx(0.774)
    assert all(a.metadata["msl_above_navd88_m"] == pytest.approx(0.774) for a in m.source("crm").assets)
    assert not m.is_fetched() and m.sea_level["egm96_grid"]["sha256"] is None


def test_no_station_makes_no_coops_request_and_no_manifest_field(tmp_path):
    http = FakeHttp({})
    m = plan_scene(parse_scene(synthetic_scene(tmp_path / "src")), tmp_path / "pkg", http=http, out=io.StringIO())
    assert http.calls == [] and m.sea_level is None
    assert "sea_level" not in json.loads((tmp_path / "pkg" / "manifest.json").read_text())


def test_feature_sources_do_not_need_footprints():
    from types import SimpleNamespace

    from camsim_scene.manifest import AssetRecord, SourceRecord
    from camsim_scene.pipeline import _needs_footprints

    asset = AssetRecord(id="socal-261009", url="https://x/socal-261009.osm.pbf", group="osm_geofabrik")
    rec = SourceRecord("osm_geofabrik", "osm_geofabrik", "OSM", "2026-10-09", "ODbL-1.0", "", {}, [asset])
    assert not _needs_footprints(SimpleNamespace(sources=[rec]))


FEATURES = {
    "area": [10.0, 10.0, 10.1, 10.04],
    "sources": ["osm_geofabrik"],
    "roads": True,
    "paved_areas": False,
    "rail": False,
    "water": False,
    "road_classes": ["motorway"],
    "rules": "1",
}


def _lc_setup(tmp_path, monkeypatch, features=None):
    from camsim_scene import pipeline
    from camsim_scene.context import BuildContext
    from camsim_scene.manifest import SourceRecord

    pkg, _, _ = build_synthetic(tmp_path)
    m = Manifest.load(pkg / "manifest.json")
    m.sources.append(SourceRecord("lc", "worldcover", "lc", "1", "CC-BY-4.0", "lc", {}, []))
    m.layers["landcover"] = {"priorities": ["lc"], "bounds": [10.0, 10.0, 10.1, 10.04], "tile_deg": 0.05}
    if features:
        m.sources.append(SourceRecord("osm_geofabrik", "osm_geofabrik", "osm", "1", "ODbL-1.0", "© OSM", {}, []))
        m.layers["landcover"]["features"] = dict(features)
    monkeypatch.setattr(
        pipeline.landcover, "local_reader", lambda paths: lambda url, r, c: np.full((600, 600), 30, np.uint8)
    )
    monkeypatch.setattr(pipeline, "_run_isolated", lambda fn, *a: fn(*a))
    road = Feature("road", "osm:w1", LineString([(10.01, 10.02), (10.04, 10.02)]), {"highway": "motorway"})
    monkeypatch.setattr(pipeline, "_load_features", lambda m, ctx: [road])
    build = lambda: pipeline._build_landcover(pkg, m, BuildContext(str(pkg), m.to_dict(), {}, {}))  # noqa: E731
    return pkg, m, build


def test_pre_r2_manifest_builds_unchanged(tmp_path, monkeypatch):
    pkg, _, build = _lc_setup(tmp_path, monkeypatch)
    build()
    index = json.loads((pkg / "landcover" / "index.json").read_text())
    assert "codes" not in index and "credits" not in index
    for t in index["tiles"]:
        assert (np.array(Image.open(pkg / "landcover" / t["file"])) == 30).all()


def test_features_burn_into_tiles_and_legend(tmp_path, monkeypatch):
    pkg, _, build = _lc_setup(tmp_path, monkeypatch, FEATURES)
    build()
    index = json.loads((pkg / "landcover" / "index.json").read_text())
    assert index["codes"] == {"200": "paved_asphalt"}
    assert len(index["credits"]) == 1 and index["credits"][0].startswith("© OSM\nLicence: Open Database License")
    burned = sum(int((np.array(Image.open(pkg / "landcover" / t["file"])) == 200).sum()) for t in index["tiles"])
    assert burned > 300  # ~3.3 km of road at ~1 texel per 7.8 m column
    attribution = (pkg / "landcover" / "ATTRIBUTION.txt").read_text()
    assert "© OSM" in attribution and "ODbL" in attribution and "OSM-derived" in attribution
    assert "CC BY 4.0" in attribution  # WorldCover's own line stays


def test_features_settings_change_rebuilds_landcover(tmp_path, monkeypatch):
    pkg, m, build = _lc_setup(tmp_path, monkeypatch, FEATURES)
    assert build().built == 2
    assert build().skipped == 2
    m.layers["landcover"]["features"]["rail"] = True
    assert build().built == 2


def test_features_pickle_through_a_spawned_process():
    road = Feature("road", "osm:w1", LineString([(10.01, 10.02), (10.04, 10.02)]), {"highway": "motorway"})
    [back] = pipeline._run_isolated(list, [road])
    assert back == road


def test_load_features_caches_the_extract(tmp_path, monkeypatch):
    from camsim_scene.context import BuildContext
    from camsim_scene.manifest import AssetRecord, SourceRecord

    asset = AssetRecord("socal", "https://x/socal.osm.pbf", "ab" * 32, 1)
    m = Manifest.load(build_synthetic(tmp_path)[0] / "manifest.json")
    m.sources.append(SourceRecord("osm_geofabrik", "osm_geofabrik", "osm", "1", "ODbL-1.0", "© OSM", {}, [asset]))
    m.layers["landcover"]["features"] = dict(FEATURES)
    road = Feature("road", "osm:w1", LineString([(10.01, 10.02), (10.04, 10.02)]), {"highway": "motorway"})
    calls = []
    monkeypatch.setattr(pipeline, "_run_isolated", lambda fn, *a: calls.append(a) or [road])
    ctx = BuildContext("pkg", m.to_dict(), {"osm_geofabrik/socal": "/x/socal.osm.pbf"}, {}, str(tmp_path / "cache"))
    assert pipeline._load_features(m, ctx) == [road]
    assert pipeline._load_features(m, ctx) == [road] and len(calls) == 1  # the second read is the cached pickle
    assert calls[0] == (FEATURES, [("osm_geofabrik", "/x/socal.osm.pbf")])
    m.layers["landcover"]["features"]["rail"] = True
    pipeline._load_features(m, ctx)
    assert len(calls) == 2  # other settings: another extract
    no_cache = BuildContext("pkg", m.to_dict(), ctx.asset_paths, {})
    pipeline._load_features(m, no_cache)
    assert len(calls) == 3


def test_buildings_need_odbl_on_the_allow_list(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    scene["buildings"] = {}
    with pytest.raises(LicenceError, match="ODbL-1.0"):
        plan_scene(parse_scene(scene), tmp_path / "pkg", http=FakeHttp({}), out=io.StringIO())


def test_buildings_source_needs_no_footprint():
    from types import SimpleNamespace

    from camsim_scene.manifest import AssetRecord, SourceRecord
    from camsim_scene.pipeline import _needs_footprints

    asset = AssetRecord(id="buildings-x", url="overture://x", group="overture_buildings")
    rec = SourceRecord("overture_buildings", "overture_buildings", "O", "x", "ODbL-1.0", "", {"release": "x"}, [asset])
    assert not _needs_footprints(SimpleNamespace(sources=[rec]))


def test_build_caps_the_gdal_cache_in_the_main_process(tmp_path, monkeypatch):
    # The buildings layer runs in the main process: without a cap GDAL's block cache is 5 % of RAM (3.1 GB on a 62 GB
    # host), which put a Pendleton build at 4.2 GB peak RSS on Linux vs 1.0 GB with the workers' 256 MB.
    monkeypatch.delenv("GDAL_CACHEMAX", raising=False)
    build_synthetic(tmp_path)
    assert os.environ["GDAL_CACHEMAX"] == pipeline.GDAL_CACHEMAX_MB


def test_build_keeps_a_gdal_cache_set_by_the_caller(tmp_path, monkeypatch):
    monkeypatch.setenv("GDAL_CACHEMAX", "64")
    build_synthetic(tmp_path)
    assert os.environ["GDAL_CACHEMAX"] == "64"
