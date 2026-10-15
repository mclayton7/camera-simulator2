import math

import numpy as np
import pytest
from shapely.geometry import LineString

from camsim_scene.bridges import Deck, deck_height, plan_decks, select
from camsim_scene.features import Feature

LAT0, LON0 = 33.0, -117.0
from camsim_scene.bridges import _scale  # noqa: E402

KX, KY = _scale(LAT0)


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


# --- Task 3: swept geometry and the GLB ---------------------------------------------------------------------------
from camsim_scene import gltf  # noqa: E402
from camsim_scene.bridges import bridges_glb, deck_mesh  # noqa: E402

ORIGIN = (LON0, LAT0, 0.0)


def flat_heights(h):
    return lambda lon, lat: np.full(np.shape(lon), float(h))


def mk_deck(pts, h0=100.0, h1=None, width=10.0, kind="road", sid="osm:w1"):
    line = LineString([ll(*p) for p in pts])
    from camsim_scene.bridges import length_m

    return Deck(sid, kind, line, width, h0, h0 if h1 is None else h1, length_m(line), 20.0)


def test_deck_top_follows_the_profile():
    d = mk_deck([(0, 0), (200, 0)], 100.0, 110.0)
    m = deck_mesh([d], ORIGIN, flat_heights(80.0))
    top = m.positions[m.roof_indices]
    assert top[:, 1].min() == pytest.approx(100.0, abs=0.02) and top[:, 1].max() == pytest.approx(110.0, abs=0.02)
    assert np.ptp(top[:, 0]) == pytest.approx(200.0, abs=0.05)
    mid = top[np.abs(top[:, 0] - 100.0) < 0.5]
    assert mid[:, 1] == pytest.approx(np.full(len(mid), 105.0), abs=0.05)


def test_cross_section_has_parapets_and_slab():
    m = deck_mesh([mk_deck([(0, 0), (200, 0)], 100.0)], ORIGIN, flat_heights(99.0))  # too low for piers
    y = m.positions[:, 1]
    assert y.max() == pytest.approx(101.0, abs=0.02) and y.min() == pytest.approx(98.5, abs=0.02)
    north = -m.positions[:, 2]
    assert np.ptp(north) == pytest.approx(10.6, abs=0.02)


def test_piers_are_centred_every_30_m_and_reach_below_terrain():
    m = deck_mesh([mk_deck([(0, 0), (200, 0)], 120.0)], ORIGIN, flat_heights(80.0))
    low = m.positions[m.positions[:, 1] < 100.0]
    assert low[:, 1].min() == pytest.approx(79.5, abs=0.02)
    xs = np.unique(np.round(low[:, 0], 1))
    centres = sorted({round(float(x), 0) for x in (xs[:-1] + xs[1:]) / 2 if True})  # noqa: C416
    pier_x = sorted({round(float(x)) for x in low[:, 0]})
    groups = np.split(np.array(pier_x), np.where(np.diff(pier_x) > 5)[0] + 1)
    assert len(groups) == 7  # (200 - 2 * 10) / 30 + 1, centred: 10, 40, ..., 190
    assert [round(float(np.mean(g))) for g in groups] == [10, 40, 70, 100, 130, 160, 190]
    del centres


def test_no_piers_where_clearance_is_small():
    m = deck_mesh([mk_deck([(0, 0), (200, 0)], 100.0)], ORIGIN, flat_heights(97.0))  # bottom 98.5: 1.5 m clear
    assert m.positions[:, 1].min() == pytest.approx(98.5, abs=0.02)


def test_sweep_handles_duplicate_points_and_hairpins():
    d = mk_deck([(0, 0), (0, 0), (50, 0), (50, 0.5), (0, 1)])
    m = deck_mesh([d], ORIGIN, flat_heights(50.0))
    assert len(m.positions) and np.isfinite(m.positions).all() and np.isfinite(m.normals).all()
    assert np.allclose(np.linalg.norm(m.normals, axis=1), 1.0, atol=1e-4)


def test_normals_point_out_of_the_slab():
    m = deck_mesh([mk_deck([(0, 0), (100, 0)], 100.0)], ORIGIN, flat_heights(99.0))
    top = m.roof_indices
    assert (m.normals[top][:, 1] > 0.99).all()  # driving surface faces up
    p, n = m.positions[m.wall_indices], m.normals[m.wall_indices]
    bottom = np.abs(p[:, 1] - 98.5) < 1e-3
    flat_bottom = bottom & (np.abs(n[:, 1]) > 0.99)
    assert flat_bottom.any() and (n[flat_bottom][:, 1] < 0).all()  # slab underside faces down


def test_feature_ids_index_decks_by_source_id():
    a, b = mk_deck([(0, 0), (100, 0)], sid="osm:w2"), mk_deck([(0, 50), (100, 50)], sid="osm:w1")
    m = deck_mesh([a, b], ORIGIN, flat_heights(50.0))
    north = -m.positions[:, 2]
    assert set(m.feature_ids[north > 25].tolist()) == {0.0}  # osm:w1 (north) sorts first
    assert set(m.feature_ids[north < 25].tolist()) == {1.0}


def test_glb_round_trips_and_is_deterministic():
    decks = [mk_deck([(0, 0), (100, 0)], sid="osm:w1"), mk_deck([(0, 50), (100, 50)], kind="rail", sid="osm:w2")]
    b1 = bridges_glb(decks, ORIGIN, flat_heights(50.0), "(c) OSM")
    assert b1 == bridges_glb(decks, ORIGIN, flat_heights(50.0), "(c) OSM")
    t = gltf.decode_feature_glb(b1)
    assert t["columns"]["source_id"] == ["osm:w1", "osm:w2"] and t["columns"]["kind"] == ["road", "rail"]
    assert t["columns"]["max_clearance_m"] == pytest.approx([20.0, 20.0])
    assert t["copyright"] == "(c) OSM" and set(t["indices"]) == {0, 1, 2}  # asphalt top, ballast top, concrete
