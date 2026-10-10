import numpy as np
import pytest
from shapely.geometry import LineString, box

from camsim_scene.features import CELL_M, PRIORITY, Feature, burn_tile, code_for, width_m

I, J = 666, -2350  # tile +33.30_-117.50
W0, S0 = J * 0.05, I * 0.05
AREA = (W0 - 1, S0 - 1, W0 + 1, S0 + 1)


def road(tags, coords=None, sid="osm:w1"):
    coords = coords or [(W0 + 0.01, S0 + 0.025), (W0 + 0.04, S0 + 0.025)]
    return Feature("road", sid, LineString(coords), {"highway": "residential", **tags})


@pytest.mark.parametrize("tags,expected", [
    ({"highway": "motorway"}, 2 * 3.5 + 3.0),
    ({"highway": "motorway", "lanes": "4"}, 4 * 3.5 + 3.0),
    ({"highway": "residential"}, 2 * 3.5 + 1.0),
    ({"highway": "residential", "oneway": "yes"}, 1 * 3.5 + 1.0),
    ({"highway": "primary", "width": "12.5"}, 12.5),
    ({"highway": "primary", "width": "12 m"}, 12.0),
    ({"highway": "primary", "lanes": "x"}, 2 * 3.5 + 1.0),
    ({"aeroway": "runway"}, 45.0),
    ({"aeroway": "taxiway"}, 23.0),
])
def test_width(tags, expected):
    assert width_m(Feature("road", "osm:w1", LineString([(0, 0), (1, 0)]), tags)) == pytest.approx(expected)


def test_rail_width_per_track():
    f = Feature("rail", "osm:w1", LineString([(0, 0), (1, 0)]), {"railway": "rail", "tracks": "2"})
    assert width_m(f) == pytest.approx(8.0)


@pytest.mark.parametrize("tags,code", [
    ({}, 200), ({"surface": "asphalt"}, 200), ({"surface": "concrete:plates"}, 201),
    ({"surface": "gravel"}, None), ({"surface": "unpaved"}, None),
    ({"bridge": "yes"}, None), ({"bridge": "no"}, 200), ({"tunnel": "culvert"}, None),
    ({"covered": "yes"}, None), ({"layer": "-1"}, None), ({"layer": "1"}, 200), ({"location": "underground"}, None),
])
def test_code_for_roads(tags, code):
    assert code_for(road(tags)) == code


def test_runway_defaults_to_concrete():
    assert code_for(Feature("road", "osm:w1", LineString([(0, 0), (1, 0)]), {"aeroway": "runway"})) == 201
    assert code_for(Feature("road", "osm:w1", LineString([(0, 0), (1, 0)]), {"aeroway": "runway", "surface": "asphalt"})) == 200


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
    f = Feature("road", "osm:w1", LineString([(c[0] - dx, c[1] - dy), (c[0] + dx, c[1] + dy)]),
                {"highway": "residential", "width": "3"})
    codes = np.full((600, 600), 30, np.uint8)
    burn_tile(codes, I, J, [f], AREA)
    m = codes == 200
    ys, xs = np.nonzero(m)
    # 8-connected and gap free: every step along the road's major axis has at least one texel
    major = ys if abs(dy) >= abs(dx * math.cos(math.radians(S0))) else xs
    assert set(range(major.min(), major.max() + 1)) <= set(major.tolist())
    # about one texel wide: across the road no line has more than 3 texels
    across = xs if major is ys else ys
    for v in np.unique(major):
        assert (major == v).sum() <= 3


def test_wide_road_burns_about_its_width():
    f = road({"width": "30"})
    codes = np.full((600, 600), 30, np.uint8)
    burn_tile(codes, I, J, [f], AREA)
    col = (codes[:, 300] == 200).sum()
    assert 3 <= col <= 4  # 30 m / 9.28 m per row


def test_priority_road_over_water_and_bridge_skipped():
    water = Feature("water", "nhd:a", box(W0 + 0.02, S0 + 0.02, W0 + 0.03, S0 + 0.03), {})
    over = road({}, sid="osm:w2")
    bridge = road({"bridge": "yes"}, [(W0 + 0.025, S0 + 0.01), (W0 + 0.025, S0 + 0.04)], sid="osm:w3")
    codes = np.full((600, 600), 30, np.uint8)
    burn_tile(codes, I, J, [bridge, over, water], AREA)
    r, c = 300, 300  # tile centre: inside the water box, on the E-W road
    assert codes[r, c] == 200
    assert codes[250, 300] == 220  # water, north of the road, where only the (skipped) bridge crosses
    assert PRIORITY["water"] < PRIORITY["road"]


def test_road_across_tile_seam_is_continuous():
    f = road({}, [(W0 + 0.04, S0 + 0.025), (W0 + 0.06, S0 + 0.025)])
    a = np.zeros((600, 600), np.uint8)
    b = np.zeros((600, 600), np.uint8)
    burn_tile(a, I, J, [f], AREA)
    burn_tile(b, I, J + 1, [f], AREA)
    assert (a[:, -1] == 200).any() and (b[:, 0] == 200).any()
    assert np.array_equal(np.nonzero(a[:, -1] == 200)[0], np.nonzero(b[:, 0] == 200)[0])


def test_burn_stops_at_margin():
    f = road({}, [(W0 + 0.01, S0 + 0.025), (W0 + 0.04, S0 + 0.025)])
    area = (W0 - 1, S0 - 1, W0 + 0.025, S0 + 1)  # margin ends half way across the tile
    codes = np.zeros((600, 600), np.uint8)
    burn_tile(codes, I, J, [f], area)
    cols = np.nonzero((codes == 200).any(axis=0))[0]
    assert cols.max() < 300 and cols.min() < 200


def test_burn_is_deterministic_regardless_of_input_order():
    fs = [road({}, sid="osm:w2"), Feature("water", "nhd:a", box(W0 + 0.02, S0 + 0.02, W0 + 0.03, S0 + 0.03), {})]
    a = np.full((600, 600), 30, np.uint8)
    b = a.copy()
    burn_tile(a, I, J, fs, AREA)
    burn_tile(b, I, J, fs[::-1], AREA)
    assert np.array_equal(a, b)
