import math

import numpy as np
import pytest
from shapely.geometry import LineString

from camsim_scene.bridges import Deck, deck_height, plan_decks, select
from camsim_scene.features import Feature

LAT0, LON0 = 33.0, -117.0
KX = 111320.0 * math.cos(math.radians(LAT0))
KY = 111320.0


def ll(x, y):
    return (LON0 + x / KX, LAT0 + y / KY)


def road(wid, pts, **tags):
    t = {"highway": "primary", "bridge": "yes", **tags}
    return Feature("road", f"osm:{wid}", LineString([ll(*p) for p in pts]), t)


def valley_at(x):
    return 100.0 - 20.0 * math.exp(-(((x - 150.0) / 50.0) ** 2))


def valley(lon, lat):
    x = (np.asarray(lon) - LON0) * KX
    return 100.0 - 20.0 * np.exp(-(((x - 150.0) / 50.0) ** 2))


def by_id(decks):
    return {d.source_id: d for d in decks}


def test_single_span_takes_end_heights_and_is_linear():
    decks, st = plan_decks([road("w1", [(0, 0), (300, 0)])], valley)
    (d,) = decks
    assert d.h0 == pytest.approx(valley_at(0), abs=1e-6) and d.h1 == pytest.approx(valley_at(300), abs=1e-6)
    assert deck_height(d, d.length_m / 2) == pytest.approx((d.h0 + d.h1) / 2)
    assert d.length_m == pytest.approx(300.0, rel=1e-3)
    assert st["kept"] == 1 and st["input"] == 1 and d.max_clearance_m > 15


def test_ramp_joining_a_viaduct_is_not_grounded():
    ws = [road("w1", [(0, 0), (150, 0)]), road("w2", [(150, 0), (300, 0)]), road("w3", [(150, 0), (150, 200)])]
    decks, _ = plan_decks(ws, valley)
    d = by_id(decks)
    hb = d["osm:w1"].h1
    assert hb > valley_at(150) + 10  # solved from the grounded ends, not the valley floor
    assert d["osm:w2"].h0 == pytest.approx(hb) and d["osm:w3"].h0 == pytest.approx(hb)
    # harmonic: hb is the 1/length-weighted mean of the three grounded ends (150, 150, 200 m)
    ends = [(valley_at(0), 150), (valley_at(300), 150), (float(valley(np.array(ll(150, 200)[0]), 0)), 200)]
    expect = sum(h / L for h, L in ends) / sum(1 / L for _, L in ends)
    assert hb == pytest.approx(expect, abs=1e-6)


def test_reversed_way_keeps_its_own_direction():
    decks, _ = plan_decks([road("w1", [(300, 0), (0, 0)])], valley)
    (d,) = decks
    assert d.h0 == pytest.approx(valley_at(300), abs=1e-6) and d.h1 == pytest.approx(valley_at(0), abs=1e-6)


def test_low_bridge_dropped():
    flat = lambda lon, lat: np.full(np.shape(lon), 50.0)  # noqa: E731
    decks, st = plan_decks([road("w1", [(0, 0), (40, 0)])], flat)
    assert decks == [] and st["dropped_low"] == 1 and st["kept"] == 0


def test_component_without_ground_dropped():
    loop = [road("w1", [(0, 0), (100, 0)]), road("w2", [(100, 0), (0, 0)])]
    decks, st = plan_decks(loop, valley)
    assert decks == [] and st["dropped_ungrounded"] == 2


def test_no_height_at_an_end_drops_the_way():
    def holes(lon, lat):
        h = valley(lon, lat)
        x = (np.asarray(lon) - LON0) * KX
        return np.where(x > 290, np.nan, h)

    decks, st = plan_decks([road("w1", [(0, 0), (300, 0)]), road("w2", [(0, 50), (250, 50)])], holes)
    assert [d.source_id for d in decks] == ["osm:w2"] and st["dropped_no_height"] == 1


def test_road_and_rail_are_separate_graphs():
    r = road("w1", [(0, 0), (150, 0)])
    rail = Feature("rail", "osm:w2", LineString([ll(150, 0), ll(300, 0)]), {"railway": "rail", "bridge": "yes"})
    decks, _ = plan_decks([r, rail], valley, min_clearance_m=-math.inf)  # keep both: this test is about grounding
    d = by_id(decks)
    assert d["osm:w1"].h1 == pytest.approx(valley_at(150), abs=1e-6)  # degree 1 in its own (road) graph: grounded


def test_select_keeps_bridges_only():
    fs = [
        road("w1", [(0, 0), (10, 0)]),
        road("w2", [(0, 0), (10, 0)], bridge="no"),
        road("w3", [(0, 0), (10, 0)], tunnel="yes"),
        road("w4", [(0, 0), (10, 0)], layer="-1"),
        road("w5", [(0, 0), (10, 0)], covered="yes"),
        Feature("water", "nhd:1", LineString([ll(0, 0), ll(1, 1)]), {"bridge": "yes"}),
    ]
    assert [f.source_id for f in select(fs)] == ["osm:w1"]


def test_deck_fields():
    decks, _ = plan_decks([road("w1", [(0, 0), (300, 0)], lanes="4")], valley)
    (d,) = decks
    assert isinstance(d, Deck) and d.kind == "road" and d.width_m == pytest.approx(4 * 3.5 + 1.0)
