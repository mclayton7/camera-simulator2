"""REALISM R5: [detail] in scene.toml -> ScenePlan, regions, layer settings and the manifest."""

from __future__ import annotations

import io
import json

import pytest
from fake_sources import synthetic_scene
from fakes import FakeHttp

from camsim_scene.config import ConfigError, layer_settings, load_scene, parse_scene
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import plan_differences, plan_scene

SQUARE = [[10.1, 10.1], [10.2, 10.1], [10.2, 10.2], [10.1, 10.2], [10.1, 10.1]]


def geojson(tmp_path, coords=SQUARE, name="a", fname="detail.geojson"):
    p = tmp_path / fname
    feat = {"type": "Feature", "properties": {"name": name}, "geometry": {"type": "Polygon", "coordinates": [coords]}}
    p.write_text(json.dumps({"type": "FeatureCollection", "features": [feat]}))
    return p


def detail_scene(tmp_path, **detail) -> dict:
    s = synthetic_scene(tmp_path / "src")
    s["profile"] = "preview"
    s["detail"] = {"file": str(geojson(tmp_path)), "scoped": {"terrain": ["hi_dem"], "imagery": ["hi_rgb"]}, **detail}
    return s


def test_detail_regions_and_settings(tmp_path):
    plan = parse_scene(detail_scene(tmp_path))
    names = [r.name for r in plan.regions()]
    assert names == ["globe", "ring", "margin", "bbox", "detail:a", "detail_margin:a"]
    assert plan.zoom["detail"] == {"terrain": 16, "imagery": 17}  # the sim profile's
    ls = layer_settings(plan)
    assert ls["terrain"]["scoped"] == {"hi_dem": "detail"} and ls["imagery"]["scoped"] == {"hi_rgb": "detail_margin"}


def test_without_detail_no_scoped_key_and_no_detail_zoom(tmp_path):
    plan = parse_scene(synthetic_scene(tmp_path / "src"))
    assert "detail" not in plan.zoom and plan.detail == {}
    ls = layer_settings(plan)
    assert "scoped" not in ls["terrain"] and "scoped" not in ls["imagery"]


def test_detail_profile_drives_default_priorities_and_ndvi(tmp_path):
    s = {
        "name": "x",
        "bbox": [10.0, 10.0, 10.5, 10.5],
        "profile": "preview",
        "detail": {"file": str(geojson(tmp_path))},
    }
    plan = parse_scene(s)
    assert plan.priorities["imagery"][0] == "naip_pc" and plan.ndvi is True
    assert plan.detail["scoped"] == {"terrain": ["dep3_1m"], "imagery": ["naip_pc"]}


def test_zoom_detail_override(tmp_path):
    s = detail_scene(tmp_path)
    s["zoom"]["detail"] = {"imagery": 15}
    assert parse_scene(s).zoom["detail"] == {"terrain": 16, "imagery": 15}


@pytest.mark.parametrize(
    "change, msg",
    [
        ({"profile": "preview2"}, "detail.profile"),
        ({"file": ""}, "detail.file"),
        ({"scoped": {"terrain": ["nope"]}}, "detail.scoped.terrain"),
        ({"scoped": {"roads": []}}, "detail.scoped must be a table"),
        ({"scoped": {"terrain": [], "imagery": []}}, "no source"),
        ({"extra": 1}, "detail must be a table"),
    ],
)
def test_detail_errors(tmp_path, change, msg):
    s = detail_scene(tmp_path)
    s["detail"].update(change)
    with pytest.raises(ConfigError, match=msg):
        parse_scene(s)


def test_sim_box_with_detail_is_an_error(tmp_path):
    s = detail_scene(tmp_path)
    s["profile"] = "sim"
    with pytest.raises(ConfigError, match="nothing to tier"):
        parse_scene(s)


def test_zoom_detail_without_detail_is_an_error(tmp_path):
    s = synthetic_scene(tmp_path / "src")
    s["zoom"]["detail"] = {"imagery": 15}
    with pytest.raises(ConfigError, match="zoom.detail"):
        parse_scene(s)


def test_file_is_relative_to_the_toml(tmp_path):
    geojson(tmp_path, fname="areas.geojson")
    (tmp_path / "s.toml").write_text(
        'name = "x"\nbbox = [10.0, 10.0, 10.5, 10.5]\nprofile = "preview"\n[detail]\nfile = "areas.geojson"\n'
    )
    assert load_scene(tmp_path / "s.toml").detail["polygons"][0]["name"] == "a"


def test_manifest_detail_section_and_absence(tmp_path):
    m = plan_scene(parse_scene(detail_scene(tmp_path)), tmp_path / "pkg", http=FakeHttp({}), out=io.StringIO())
    assert m.detail["profile"] == "sim" and m.detail["polygons"][0]["name"] == "a" and len(m.detail["sha256"]) == 64
    assert Manifest.load(tmp_path / "pkg/manifest.json").detail == m.detail
    m0 = plan_scene(
        parse_scene(synthetic_scene(tmp_path / "src")), tmp_path / "p0", http=FakeHttp({}), out=io.StringIO()
    )
    assert m0.detail is None and "detail" not in json.loads((tmp_path / "p0/manifest.json").read_text())


def test_changed_polygons_need_replan(tmp_path):
    s = detail_scene(tmp_path)
    plan_scene(parse_scene(s), tmp_path / "pkg", http=FakeHttp({}), out=io.StringIO())
    geojson(tmp_path, coords=[[x + 0.05, y] for x, y in SQUARE])
    m = Manifest.load(tmp_path / "pkg/manifest.json")
    assert "regions" in plan_differences(parse_scene(s), m)
