import pytest
import shapely

from camsim_scene import config, tiling
from camsim_scene.config import ConfigError, parse_scene

PENDLETON = [-117.62, 33.19, -117.24, 33.52]


def test_defaults_for_sim_profile():
    p = parse_scene({"name": "pendleton", "bbox": PENDLETON})
    assert p.profile == "sim" and p.ring_km == 100 and p.bmng_month == 7 and p.jpeg_quality == 85
    assert p.priorities["terrain"] == ["dep3_1m", "dep3_13", "etopo2022"]
    assert p.priorities["imagery"] == ["naip_pc", "wc_s2", "bmng"]
    assert p.priorities["landcover"] == ["worldcover"]
    assert p.source_options["bmng"]["month"] == 7 and p.source_options["naip_pc"]["year"] == "2022"
    assert [r.name for r in p.regions()] == ["globe", "ring", "bbox"]
    assert [r.max_zoom for r in p.regions()] == [
        {"terrain": 8, "imagery": 8},
        {"terrain": 10, "imagery": 10},
        {"terrain": 16, "imagery": 17},
    ]
    assert p.source_ids() == ["dep3_1m", "dep3_13", "etopo2022", "naip_pc", "wc_s2", "bmng", "worldcover"]


def test_preview_profile_has_no_naip_or_1m():
    p = parse_scene({"name": "pv", "bbox": PENDLETON, "profile": "preview"})
    assert p.priorities["terrain"] == ["dep3_13", "etopo2022"] and p.priorities["imagery"] == ["wc_s2", "bmng"]
    assert p.regions()[2].max_zoom == {"terrain": 14, "imagery": 13}


def test_ring_is_100_km_at_pendleton():
    w, s, e, n = config.ring_bounds(tuple(PENDLETON), 100.0)
    assert s == pytest.approx(33.19 - 100 / 111.32) and n == pytest.approx(33.52 + 100 / 111.32)
    assert 1.05 < (-117.62 - w) < 1.15 and 1.05 < (e + 117.24) < 1.15


def test_zoom_and_priority_overrides():
    p = parse_scene(
        {
            "name": "t",
            "bbox": [10, 10, 10.5, 10.5],
            "priorities": {"terrain": ["a"], "imagery": ["b"], "landcover": []},
            "sources": {"a": {"adapter": "x:Y", "k": 1}},
            "zoom": {"globe": {"terrain": 2, "imagery": 2}, "bbox": {"terrain": 6}},
        }
    )
    assert p.priorities["landcover"] == [] and p.source_options["a"] == {"adapter": "x:Y", "k": 1}
    assert p.zoom["globe"] == {"terrain": 2, "imagery": 2} and p.zoom["bbox"] == {"terrain": 6, "imagery": 17}


@pytest.mark.parametrize(
    "patch, match",
    [
        ({"bbox": [10, 0, 5, 1]}, "antimeridian"),
        ({"bbox": [0, 5, 1, 4]}, "S < N"),
        ({"bbox": [0, 0, 1, 91]}, "outside"),
        ({"bbox": [0, 0, 1]}, "4 numbers"),
        ({"profile": "ultra"}, "profile"),
        ({"bmng_month": 13}, "bmng_month"),
        ({"jpeg_quality": 0}, "jpeg_quality"),
        ({"name": "Bad Name"}, "name"),
        ({"colour": "red"}, "unknown key"),
        ({"priorities": {"terrain": "dep3_13"}}, "list"),
    ],
)
def test_invalid_scenes_are_rejected(patch, match):
    with pytest.raises(ConfigError, match=match):
        parse_scene({"name": "ok", "bbox": PENDLETON, **patch})


def test_bbox_crossing_antimeridian_is_rejected():
    with pytest.raises(ConfigError, match="antimeridian"):
        parse_scene({"name": "fiji", "bbox": [177.0, -19.0, -179.0, -16.0]})


def test_tiny_bbox_plans_to_z17():
    p = parse_scene({"name": "tiny", "bbox": [-117.380001, 33.220001, -117.380000, 33.220002]})
    cov = tiling.Coverage([shapely.box(*tiling.GLOBE)], [17])
    plan = tiling.plan_tiles(p.regions(), "imagery", cov)
    assert plan.max_zoom == 17 and len(plan.tiles[17]) == 4


def test_tiling_names_the_ogc_tile_matrix_set():
    assert config.TILING["tile_matrix_set"] == "WorldCRS84Quad" and config.TILING["root_tiles"] == [2, 1]


def test_layer_settings_carry_quality_and_priorities():
    ls = config.layer_settings(parse_scene({"name": "p", "bbox": PENDLETON, "jpeg_quality": 80}))
    assert ls["imagery"]["jpeg_quality"] == 80 and ls["terrain"]["grid"] == 257 and ls["terrain"]["feather_m"] == 30.0
    assert ls["landcover"]["priorities"] == ["worldcover"] and len(ls["landcover"]["bounds"]) == 4


def test_default_scene_toml_round_trips(tmp_path):
    path = tmp_path / "pendleton.scene.toml"
    path.write_text(config.default_scene_toml("pendleton", tuple(PENDLETON), "sim"))
    p = config.load_scene(path)
    assert p.name == "pendleton" and p.bbox == tuple(PENDLETON) and p.profile == "sim"
