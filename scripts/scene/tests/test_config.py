import pytest
import shapely

from camsim_scene import config, tiling
from camsim_scene.config import ConfigError, layer_settings, parse_scene

PENDLETON = [-117.62, 33.19, -117.24, 33.52]


def test_defaults_for_sim_profile():
    p = parse_scene({"name": "pendleton", "bbox": PENDLETON})
    assert p.profile == "sim" and p.ring_km == 100 and p.bmng_month == 7 and p.jpeg_quality == 85
    assert p.priorities["terrain"] == ["dep3_1m", "dep3_13", "noaa_sd13", "noaa_crm_socal", "etopo2022"]
    assert p.priorities["imagery"] == ["naip_pc", "wc_s2", "bmng"]
    assert p.priorities["landcover"] == ["worldcover"]
    assert p.source_options["bmng"]["month"] == 7 and p.source_options["naip_pc"]["year"] == "2022"
    assert [r.name for r in p.regions()] == ["globe", "ring", "margin", "bbox"]
    assert [r.max_zoom for r in p.regions()] == [
        {"terrain": 8, "imagery": 8},
        {"terrain": 10, "imagery": 10},
        {"terrain": 10, "imagery": 17},
        {"terrain": 16, "imagery": 17},
    ]
    assert p.source_ids() == [
        "dep3_1m",
        "dep3_13",
        "noaa_sd13",
        "noaa_crm_socal",
        "etopo2022",
        "naip_pc",
        "wc_s2",
        "bmng",
        "worldcover",
    ]


def test_preview_profile_has_no_naip_or_1m():
    p = parse_scene({"name": "pv", "bbox": PENDLETON, "profile": "preview"})
    assert p.priorities["terrain"] == ["dep3_13", "noaa_sd13", "noaa_crm_socal", "etopo2022"] and p.priorities[
        "imagery"
    ] == ["wc_s2", "bmng"]
    assert p.regions()[3].max_zoom == {"terrain": 14, "imagery": 13}


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
    p = parse_scene({"name": "tiny", "bbox": [-117.380001, 33.220001, -117.380000, 33.220002], "imagery_margin_km": 0})
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


def test_margin_region_sits_between_ring_and_bbox():
    p = config.parse_scene({"name": "t", "bbox": PENDLETON})
    assert p.imagery_margin_km == 3.0 and p.balance is True
    assert [r.name for r in p.regions()] == ["globe", "ring", "margin", "bbox"]
    assert p.regions()[2].max_zoom == {"terrain": 10, "imagery": 17}
    w, s, e, n = p.area("margin")
    assert s == pytest.approx(PENDLETON[1] - 3.0 / config.KM_PER_DEG)
    assert n == pytest.approx(PENDLETON[3] + 3.0 / config.KM_PER_DEG)
    assert w < PENDLETON[0] and e > PENDLETON[2]


def test_zero_margin_is_the_bbox():
    p = config.parse_scene({"name": "t", "bbox": PENDLETON, "imagery_margin_km": 0})
    assert p.area("margin") == p.bbox


@pytest.mark.parametrize(
    "bad",
    [
        {"imagery_margin_km": -1},
        {"imagery_margin_km": 51},
        {"imagery_margin_km": 20, "ring_km": 10},
        {"balance": "yes"},
    ],
)
def test_bad_margin_or_balance(bad):
    with pytest.raises(config.ConfigError):
        config.parse_scene({"name": "t", "bbox": PENDLETON, **bad})


def test_balance_settings_need_naip_and_s2():
    sim = config.layer_settings(config.parse_scene({"name": "t", "bbox": PENDLETON}))["imagery"]
    assert sim["margin_km"] == 3.0
    assert sim["balance"]["reference"] == "naip_pc" and sim["balance"]["target"] == "wc_s2"
    assert sim["balance"]["feather_m"] == 200.0 and sim["balance"]["exclude_classes"] == [0, 80]
    off = config.parse_scene({"name": "t", "bbox": PENDLETON, "balance": False})
    assert config.layer_settings(off)["imagery"]["balance"] is None
    preview = config.parse_scene({"name": "t", "bbox": PENDLETON, "profile": "preview"})
    assert config.layer_settings(preview)["imagery"]["balance"] is None


def test_naip_discovers_over_the_margin():
    from camsim_scene.sources.naip_pc import NaipPc

    assert NaipPc.area_kind == "margin"


def test_water_buffer_defaults_and_settings():
    p = config.parse_scene({"name": "t", "bbox": PENDLETON})
    assert p.naip_water_buffer_m == 200.0
    s = config.layer_settings(p)["imagery"]["balance"]
    assert s["naip_water_buffer_m"] == 200.0 and s["water_fade_m"] == 100.0
    off = config.parse_scene({"name": "t", "bbox": PENDLETON, "naip_water_buffer_m": 0})
    assert config.layer_settings(off)["imagery"]["balance"]["naip_water_buffer_m"] == 0.0
    assert config.parse_scene({"name": "t", "bbox": PENDLETON, "naip_water_buffer_m": 500}).naip_water_buffer_m == 500.0
    nob = config.parse_scene({"name": "t", "bbox": PENDLETON, "balance": False, "naip_water_buffer_m": 500})
    assert config.layer_settings(nob)["imagery"]["balance"] is None


@pytest.mark.parametrize("bad", [-1, 100, 199.9, 5001, "200", True])
def test_bad_water_buffer(bad):
    with pytest.raises(config.ConfigError):
        config.parse_scene({"name": "t", "bbox": PENDLETON, "naip_water_buffer_m": bad})


def test_ndvi_defaults_per_profile():
    sim = parse_scene({"name": "t", "bbox": PENDLETON})
    assert sim.ndvi is True and sim.ndvi_max_zoom == 15
    s = config.layer_settings(sim)["ndvi"]
    assert s["priorities"] == ["naip_pc", "wc_s2"] and s["reference"] == "naip_pc" and s["target"] == "wc_s2"
    assert s["max_zoom"] == 15 and s["feather_m"] == 200.0 and s["naip_water_buffer_m"] == 200.0
    assert s["fit_step_m"] == 10.0 and s["min_samples"] == 500 and s["exclude_classes"] == [0, 80]
    prev = parse_scene({"name": "t", "bbox": PENDLETON, "profile": "preview"})
    assert prev.ndvi is False and "ndvi" not in config.layer_settings(prev)


def test_ndvi_off_leaves_the_other_layers_unchanged():
    on, off = (config.layer_settings(parse_scene({"name": "t", "bbox": PENDLETON, "ndvi": v})) for v in (True, False))
    assert "ndvi" not in off and {k: v for k, v in on.items() if k != "ndvi"} == off


def test_preview_can_turn_ndvi_on_with_sentinel2_only():
    plan = parse_scene({"name": "t", "bbox": PENDLETON, "profile": "preview", "ndvi": True, "ndvi_max_zoom": 12})
    s = config.layer_settings(plan)["ndvi"]
    assert s["priorities"] == ["wc_s2"] and s["reference"] is None and s["target"] == "wc_s2" and s["max_zoom"] == 12


def test_default_ndvi_is_off_without_ndvi_sources():
    plan = parse_scene({"name": "t", "bbox": PENDLETON, "priorities": {"imagery": ["bmng"]}})
    assert "ndvi" not in config.layer_settings(plan)


@pytest.mark.parametrize(
    "patch, match",
    [
        ({"ndvi": "yes"}, "ndvi must be true or false"),
        ({"ndvi_max_zoom": 9}, "ndvi_max_zoom"),
        ({"ndvi_max_zoom": 18}, "ndvi_max_zoom"),
        ({"ndvi_max_zoom": 15.0}, "ndvi_max_zoom"),
        ({"ndvi": True, "priorities": {"imagery": ["bmng"]}}, "needs naip_pc or wc_s2"),
    ],
)
def test_bad_ndvi(patch, match):
    with pytest.raises(ConfigError, match=match):
        parse_scene({"name": "t", "bbox": PENDLETON, **patch})


def test_sea_level_station_parses():
    p = parse_scene({"name": "x", "bbox": [-117.6, 33.2, -117.2, 33.5], "sea_level": {"station": "9410230"}})
    assert p.sea_level_station == "9410230"


@pytest.mark.parametrize("bad", [{"station": 9410230}, {"station": ""}, {"station": "x1"}, {"stn": "9410230"}])
def test_sea_level_station_rejects_bad_values(bad):
    with pytest.raises(ConfigError, match="sea_level"):
        parse_scene({"name": "x", "bbox": [-117.6, 33.2, -117.2, 33.5], "sea_level": bad})


def test_sim_terrain_priorities_and_tidal_mask():
    p = parse_scene({"name": "x", "bbox": [-117.6, 33.2, -117.2, 33.5]})
    assert p.priorities["terrain"] == ["dep3_1m", "dep3_13", "noaa_sd13", "noaa_crm_socal", "etopo2022"]
    assert layer_settings(p)["terrain"]["tidal_mask"] == {
        "classes_source": "worldcover",
        "class": 80,
        "max_navd88_m": 1.5,
        "sources": ["dep3_1m", "dep3_13"],
        "topobathy": ["noaa_sd13", "noaa_crm_socal"],
    }


def test_no_tidal_mask_without_topobathy():
    p = parse_scene(
        {"name": "x", "bbox": [-117.6, 33.2, -117.2, 33.5], "priorities": {"terrain": ["dep3_13", "etopo2022"]}}
    )
    assert "tidal_mask" not in layer_settings(p)["terrain"]


def test_every_example_with_a_crm_ring_has_a_station():
    # plan raises PlanError when noaa_crm_socal discovers assets and there is no [sea_level] station;
    # the CRM covers lon -128..-115, lat 30..37 (docs/scene-packages.md), so an example whose bbox or ring meets
    # it needs one. No network: only the parsed config is checked.
    from pathlib import Path

    crm = (-128.0, 30.0, -115.0, 37.0)
    examples = sorted((Path(__file__).parent.parent / "examples").glob("*.toml"))
    assert examples
    for f in examples:
        plan = config.load_scene(f)
        if "noaa_crm_socal" not in plan.source_ids():
            continue
        w, s, e, n = plan.ring_bounds()
        if w < crm[2] and e > crm[0] and s < crm[3] and n > crm[1]:
            assert plan.sea_level_station, f"{f.name}: its ring meets the CRM, so it needs [sea_level] station"


BASE = {"name": "t", "bbox": [-117.5, 33.2, -117.4, 33.3]}


def test_features_default_off_keeps_layer_settings():
    plan = parse_scene(dict(BASE))
    assert plan.priorities["features"] == []
    assert config.features_settings(plan) is None
    assert "features" not in layer_settings(plan)["landcover"]


def test_features_flags_set_priorities_and_settings():
    plan = parse_scene({**BASE, "allow": ["ODbL-1.0"], "features": {"roads": True, "water": True}})
    assert plan.priorities["features"] == ["nhd_hr", "osm_geofabrik"]
    s = layer_settings(plan)["landcover"]["features"]
    assert s["roads"] and s["water"] and not s["rail"] and not s["paved_areas"]
    assert s["sources"] == ["nhd_hr", "osm_geofabrik"]
    assert s["area"] == list(plan.area("margin"))
    assert s["road_classes"][0] == "motorway"


def test_water_only_needs_no_osm():
    plan = parse_scene({**BASE, "features": {"water": True}})
    assert plan.priorities["features"] == ["nhd_hr"]


@pytest.mark.parametrize("bad", [{"roads": 1}, {"bogus": True}, {"road_classes": ["motorway", "footway"]}])
def test_features_validation(bad):
    with pytest.raises(ConfigError):
        parse_scene({**BASE, "features": bad})


def test_features_not_settable_through_priorities():
    with pytest.raises(ConfigError):
        parse_scene({**BASE, "priorities": {"features": ["osm_geofabrik"]}})


BUILDINGS_TOML = {"name": "t", "bbox": [10.0, 10.0, 10.1, 10.1]}


def test_no_buildings_section_leaves_layers_unchanged():
    plan = parse_scene(dict(BUILDINGS_TOML))
    assert plan.buildings == {} and plan.priorities["buildings"] == []
    assert "buildings" not in layer_settings(plan)


def test_buildings_section_defaults_and_settings():
    plan = parse_scene({**BUILDINGS_TOML, "allow": ["ODbL-1.0"], "buildings": {}})
    assert plan.buildings == {"enabled": True, "release": "2026-09-23.1", "min_area_m2": 10.0}
    assert plan.priorities["buildings"] == ["overture_buildings"]
    assert plan.source_options["overture_buildings"] == {"release": "2026-09-23.1"}
    s = layer_settings(plan)["buildings"]
    assert s["tiers"] == {"13": 2000.0, "14": 300.0} and s["leaf_zoom"] == 15
    assert s["geometric_error"] == {"root": 200.0, "13": 60.0, "14": 20.0, "15": 6.0}


def test_buildings_disabled_is_no_layer():
    plan = parse_scene({**BUILDINGS_TOML, "buildings": {"enabled": False}})
    assert plan.priorities["buildings"] == [] and "buildings" not in layer_settings(plan)


@pytest.mark.parametrize(
    "bad",
    [{"enabled": "yes"}, {"release": ""}, {"release": 5}, {"min_area_m2": -1}, {"min_area_m2": True}, {"colour": 1}],
)
def test_buildings_section_rejects_bad_values(bad):
    with pytest.raises(ConfigError, match="buildings"):
        parse_scene({**BUILDINGS_TOML, "buildings": bad})


def test_priorities_buildings_cannot_be_set_directly():
    with pytest.raises(ConfigError, match="buildings"):
        parse_scene({**BUILDINGS_TOML, "priorities": {"buildings": ["overture_buildings"]}})
