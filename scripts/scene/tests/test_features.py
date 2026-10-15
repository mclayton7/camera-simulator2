import numpy as np
import pytest
from scipy.ndimage import label
from shapely.geometry import LineString, box

from camsim_scene.features import PRIORITY, Feature, burn_tile, code_for, width_m

TI, TJ = 666, -2350  # tile +33.30_-117.50
W0, S0 = TJ * 0.05, TI * 0.05
AREA = (W0 - 1, S0 - 1, W0 + 1, S0 + 1)


def road(tags, coords=None, sid="osm:w1"):
    coords = coords or [(W0 + 0.01, S0 + 0.025), (W0 + 0.04, S0 + 0.025)]
    return Feature("road", sid, LineString(coords), {"highway": "residential", **tags})


@pytest.mark.parametrize(
    "tags,expected",
    [
        ({"highway": "motorway"}, 2 * 3.5 + 3.0),
        ({"highway": "motorway", "lanes": "4"}, 4 * 3.5 + 3.0),
        ({"highway": "residential"}, 2 * 3.5 + 1.0),
        ({"highway": "residential", "oneway": "yes"}, 1 * 3.5 + 1.0),
        ({"highway": "primary", "width": "12.5"}, 12.5),
        ({"highway": "primary", "width": "12 m"}, 12.0),
        ({"highway": "primary", "lanes": "x"}, 2 * 3.5 + 1.0),
        ({"aeroway": "runway"}, 45.0),
        ({"aeroway": "taxiway"}, 23.0),
    ],
)
def test_width(tags, expected):
    assert width_m(Feature("road", "osm:w1", LineString([(0, 0), (1, 0)]), tags)) == pytest.approx(expected)


def test_rail_width_per_track():
    f = Feature("rail", "osm:w1", LineString([(0, 0), (1, 0)]), {"railway": "rail", "tracks": "2"})
    assert width_m(f) == pytest.approx(8.0)


@pytest.mark.parametrize(
    "tags,code",
    [
        ({}, 200),
        ({"surface": "asphalt"}, 200),
        ({"surface": "concrete:plates"}, 201),
        ({"surface": "gravel"}, None),
        ({"surface": "unpaved"}, None),
        ({"bridge": "yes"}, None),
        ({"bridge": "no"}, 200),
        ({"tunnel": "culvert"}, None),
        ({"covered": "yes"}, None),
        ({"layer": "-1"}, None),
        ({"layer": "1"}, 200),
        ({"location": "underground"}, None),
    ],
)
def test_code_for_roads(tags, code):
    assert code_for(road(tags)) == code


def test_runway_defaults_to_concrete():
    assert code_for(Feature("road", "osm:w1", LineString([(0, 0), (1, 0)]), {"aeroway": "runway"})) == 201
    assert (
        code_for(Feature("road", "osm:w1", LineString([(0, 0), (1, 0)]), {"aeroway": "runway", "surface": "asphalt"}))
        == 200
    )


def test_rail_and_water_codes():
    assert code_for(Feature("rail", "osm:w1", LineString([(0, 0), (1, 0)]), {"railway": "rail"})) == 210
    assert code_for(Feature("water", "nhd:x", box(0, 0, 1, 1), {})) == 220


def _strip_runs(mask: np.ndarray, axis: int) -> list[int]:
    """Texel count of the mask in each line across the road (rows when the road runs E-W)."""
    return [int(v) for v in mask.sum(axis=axis) if v]


@pytest.mark.parametrize("bearing", [0, 30, 45, 60, 90])
def test_narrow_road_is_a_continuous_one_texel_strip(bearing):
    import math

    c = (W0 + 0.025, S0 + 0.025)
    d = 0.015
    dx, dy = d * math.sin(math.radians(bearing)) / math.cos(math.radians(S0)), d * math.cos(math.radians(bearing))
    f = Feature(
        "road",
        "osm:w1",
        LineString([(c[0] - dx, c[1] - dy), (c[0] + dx, c[1] + dy)]),
        {"highway": "residential", "width": "3"},
    )
    codes = np.full((600, 600), 30, np.uint8)
    burn_tile(codes, TI, TJ, [f], AREA)
    m = codes == 200
    ys, xs = np.nonzero(m)
    # one 8-connected component, no gaps
    _, n = label(m, structure=np.ones((3, 3)))
    assert n == 1
    # about one texel wide: pick the major axis in pixels (cells are ~7.8 m E-W x 9.3 m N-S)
    px_dx = dx * math.cos(math.radians(S0)) * 111_320.0 / (111_320.0 / 12000 * math.cos(math.radians(S0)))
    px_dy = dy * 12000
    major = ys if abs(px_dy) >= abs(px_dx) else xs
    for v in np.unique(major):
        assert (major == v).sum() <= 2


def test_wide_road_burns_about_its_width():
    f = road({"width": "30"})
    codes = np.full((600, 600), 30, np.uint8)
    burn_tile(codes, TI, TJ, [f], AREA)
    col = (codes[:, 300] == 200).sum()
    assert 3 <= col <= 4  # 30 m / 9.28 m per row


def test_priority_road_over_water_and_bridge_skipped():
    water = Feature("water", "nhd:a", box(W0 + 0.02, S0 + 0.02, W0 + 0.03, S0 + 0.03), {})
    over = road({}, sid="osm:w2")
    bridge = road({"bridge": "yes"}, [(W0 + 0.025, S0 + 0.01), (W0 + 0.025, S0 + 0.04)], sid="osm:w3")
    codes = np.full((600, 600), 30, np.uint8)
    burn_tile(codes, TI, TJ, [bridge, over, water], AREA)
    r, c = 300, 300  # tile centre: inside the water box, on the E-W road
    assert codes[r, c] == 200
    assert codes[250, 300] == 220  # water, north of the road, where only the (skipped) bridge crosses
    assert PRIORITY["water"] < PRIORITY["road"]


def test_road_across_tile_seam_is_continuous():
    f = road({}, [(W0 + 0.04, S0 + 0.025), (W0 + 0.06, S0 + 0.025)])
    a = np.zeros((600, 600), np.uint8)
    b = np.zeros((600, 600), np.uint8)
    burn_tile(a, TI, TJ, [f], AREA)
    burn_tile(b, TI, TJ + 1, [f], AREA)
    assert (a[:, -1] == 200).any() and (b[:, 0] == 200).any()
    assert np.array_equal(np.nonzero(a[:, -1] == 200)[0], np.nonzero(b[:, 0] == 200)[0])


def test_burn_stops_at_margin():
    f = road({}, [(W0 + 0.01, S0 + 0.025), (W0 + 0.04, S0 + 0.025)])
    area = (W0 - 1, S0 - 1, W0 + 0.025, S0 + 1)  # margin ends half way across the tile
    codes = np.zeros((600, 600), np.uint8)
    burn_tile(codes, TI, TJ, [f], area)
    cols = np.nonzero((codes == 200).any(axis=0))[0]
    assert cols.max() < 300 and cols.min() < 200


def test_burn_is_deterministic_regardless_of_input_order():
    fs = [road({}, sid="osm:w2"), Feature("water", "nhd:a", box(W0 + 0.02, S0 + 0.02, W0 + 0.03, S0 + 0.03), {})]
    a = np.full((600, 600), 30, np.uint8)
    b = a.copy()
    burn_tile(a, TI, TJ, fs, AREA)
    burn_tile(b, TI, TJ, fs[::-1], AREA)
    assert np.array_equal(a, b)


def test_far_feature_is_skipped_without_buffering(monkeypatch):
    import camsim_scene.features as F

    calls = []
    orig = F._polygon
    monkeypatch.setattr(F, "_polygon", lambda f, local: calls.append(f) or orig(f, local))
    far = road({}, [(W0 + 0.5, S0 + 0.5), (W0 + 0.6, S0 + 0.5)], sid="osm:far")
    near = road({}, sid="osm:near")
    codes = np.zeros((600, 600), np.uint8)
    burn_tile(codes, TI, TJ, [far, near], AREA)
    assert [f.source_id for f in calls] == ["osm:near"]


def _rail_at_col(col, sid="osm:w9"):
    x = W0 + (col + 0.5) / 12000.0
    return Feature("rail", sid, LineString([(x, S0 + 0.005), (x, S0 + 0.045)]), {"railway": "rail"})


def test_buildings_burn_between_paved_areas_and_rail():
    from camsim_scene.features import BUILDING_CODE

    paved = Feature("paved_area", "osm:a1", box(W0 + 0.005, S0 + 0.01, W0 + 0.02, S0 + 0.04), {"amenity": "parking"})
    over = road({}, sid="osm:w2")  # E-W along row 300
    rail = _rail_at_col(400)
    bmask = np.zeros((600, 600), bool)
    bmask[100:500, 100:500] = True
    codes = np.full((600, 600), 30, np.uint8)
    burn_tile(codes, TI, TJ, [rail, over, paved], AREA, bmask)
    assert BUILDING_CODE == 230
    assert codes[200, 150] == 230  # a paved area under a building becomes building (parking structures)
    assert codes[200, 70] == 200  # the paved area outside the building stays paved
    assert codes[300, 150] == 200  # the road over the building wins
    assert codes[200, 400] == 210  # the rail over the building wins
    assert codes[200, 450] == 230 and codes[50, 50] == 30 and codes[200, 550] == 30
    assert PRIORITY["paved_area"] < PRIORITY["building"] < PRIORITY["rail"] < PRIORITY["road"]


def test_buildings_none_or_empty_burns_features_exactly_as_before():
    feats = [road({}, sid="osm:w2"), _rail_at_col(400), Feature("water", "nhd:a", box(W0 + 0.02, S0 + 0.02, W0 + 0.03, S0 + 0.03), {})]
    a = np.full((600, 600), 30, np.uint8)
    b = a.copy()
    na = burn_tile(a, TI, TJ, feats, AREA)
    nb = burn_tile(b, TI, TJ, feats, AREA, np.zeros((600, 600), bool))
    assert na == nb and np.array_equal(a, b)


def test_buildings_alone_need_no_features_or_area():
    bmask = np.zeros((600, 600), bool)
    bmask[10:12, 20:23] = True
    codes = np.full((600, 600), 10, np.uint8)
    assert burn_tile(codes, TI, TJ, [], None, bmask) == 6
    assert (codes == 230).sum() == 6


def test_bridges_on_burns_bridge_ways_but_not_tunnels():
    from camsim_scene.features import burnable_with_bridges

    br = road({"bridge": "yes"}, [(W0 + 0.025, S0 + 0.01), (W0 + 0.025, S0 + 0.04)], sid="osm:w3")
    tun = road({"tunnel": "yes", "bridge": "yes"}, [(W0 + 0.03, S0 + 0.01), (W0 + 0.03, S0 + 0.04)], sid="osm:w4")
    out = burnable_with_bridges([br, tun])
    assert [code_for(f) for f in out] == [200, None]
    assert br.tags["bridge"] == "yes"  # the input is not mutated
    codes = np.full((600, 600), 30, np.uint8)
    burn_tile(codes, TI, TJ, out, AREA)
    assert codes[250, 300] == 200  # where the bridge crosses
