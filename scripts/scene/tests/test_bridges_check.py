import math
import sys
from pathlib import Path

import pytest
from shapely.geometry import LineString

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import bridges_check as bc  # noqa: E402

from camsim_scene.bridges import Deck, _scale, length_m  # noqa: E402
from camsim_scene.features import Feature  # noqa: E402

LAT0, LON0 = 33.0, -117.0
KX, KY = _scale(LAT0)


def ll(x, y):
    return (LON0 + x / KX, LAT0 + y / KY)


def deck(sid, pts, h0=100.0, h1=110.0, clear=20.0, kind="road"):
    line = LineString([ll(*p) for p in pts])
    return Deck(sid, kind, line, 10.0, h0, h1, length_m(line), clear)


def test_pick_highest_clearance_first():
    ds = [deck("osm:w1", [(0, 0), (100, 0)], clear=5), deck("osm:w2", [(0, 0), (100, 0)], clear=30),
          deck("osm:w3", [(0, 0), (100, 0)], clear=30)]
    assert [d.source_id for d in bc.pick(ds, 2)] == ["osm:w2", "osm:w3"]


def test_midspan_is_halfway_at_the_mean_height():
    lat, lon, h = bc.midspan(deck("osm:w1", [(0, 0), (200, 0)]))
    x = (lon - LON0) * KX
    assert x == pytest.approx(100.0, abs=0.01) and h == pytest.approx(105.0)


def test_grounded_ends_step_outward():
    d = deck("osm:w1", [(0, 0), (200, 0)])
    ends = bc.grounded_ends(d, {("osm:w1", 0): True, ("osm:w1", 1): False})
    assert len(ends) == 1
    e = ends[0]
    assert e["h"] == pytest.approx(100.0)
    bx = (e["beyond"][1] - LON0) * KX
    assert bx == pytest.approx(-5.0, abs=0.01)


def test_evaluate_b2():
    assert bc.evaluate_b2({"hot": 105.05, "material": 200}, 105.0)["pass"]
    assert not bc.evaluate_b2({"hot": 105.2, "material": 200}, 105.0)["pass"]
    assert not bc.evaluate_b2({"hot": 105.0, "material": 80}, 105.0)["pass"]


def test_evaluate_b3():
    assert bc.evaluate_b3(100.0, 100.4)["pass"] and not bc.evaluate_b3(100.0, 100.6)["pass"]


def test_evaluate_drive_over():
    d = deck("osm:w1", [(0, 0), (200, 0)], 100.0, 100.0)
    track = [(s, 100.0 + 0.01 * (s % 2)) for s in range(-50, 251, 2)]
    r = bc.evaluate_over(track, d)
    assert r["pass"] and r["max_step_m"] < 0.3
    bad = [(s, 100.0 if s < 100 else 80.0) for s in range(-50, 251, 2)]
    assert not bc.evaluate_over(bad, d)["pass"]


def test_evaluate_under():
    assert bc.evaluate_under([80.0, 80.1, 80.0], deck_top=100.0)["pass"]
    assert not bc.evaluate_under([80.0, 99.9, 80.0], deck_top=100.0)["pass"]


def test_find_underpass():
    d = deck("osm:w1", [(0, 0), (200, 0)], clear=20)
    road = Feature("road", "osm:w9", LineString([ll(100, -100), ll(100, 100)]), {"highway": "primary"})
    over = Feature("road", "osm:w8", LineString([ll(50, -100), ll(50, 100)]), {"highway": "primary", "bridge": "yes"})
    u = bc.find_underpass([d], [over, road])
    assert u is not None and u["road"] == "osm:w9" and u["deck"] == "osm:w1"
    x = (u["crossing"][1] - LON0) * KX
    assert x == pytest.approx(100.0, abs=0.01)
    assert bc.find_underpass([d], [over]) is None


def test_track_along_extends_past_both_ends():
    d = deck("osm:w1", [(0, 0), (200, 0)])
    pts = bc.track_along(d.line, beyond_m=50.0, step_m=10.0)
    xs = [(lon - LON0) * KX for _, lon, _ in pts]
    assert xs[0] == pytest.approx(-50.0, abs=0.05) and xs[-1] == pytest.approx(250.0, abs=0.05)
    assert all(math.isclose(h, 90.0, abs_tol=0.5) for *_, h in pts)  # heading east


def test_find_underpass_ignores_roads_meeting_the_deck_end():
    d = deck("osm:w1", [(0, 0), (200, 0)], clear=20)
    approach = Feature("road", "osm:w7", LineString([ll(200, 0), ll(400, 0)]), {"highway": "primary"})
    side = Feature("road", "osm:w6", LineString([ll(195, -50), ll(195, 50)]), {"highway": "primary"})
    assert bc.find_underpass([d], [approach, side]) is None


def test_grounded_by_the_osm_way_ends_not_the_extended_deck_line():
    f = Feature("road", "osm:w1", LineString([ll(0, 0), ll(200, 0)]), {"highway": "primary", "bridge": "yes"})
    g = Feature("road", "osm:w2", LineString([ll(200, 0), ll(300, 50)]), {"highway": "primary", "bridge": "yes"})
    assert bc.grounded(([f, g])) == {("osm:w1", 0): True, ("osm:w1", 1): False, ("osm:w2", 0): False,
                                     ("osm:w2", 1): True}


def test_find_underpass_ignores_the_approach_even_on_an_extended_deck():
    osm = Feature("road", "osm:w1", LineString([ll(0, 0), ll(200, 0)]), {"highway": "primary", "bridge": "yes"})
    d = deck("osm:w1", [(-25, 0), (225, 0)], clear=20)  # extended 25 m past both OSM ends
    approach = Feature("road", "osm:w7", LineString([ll(-100, 30), ll(0, 0)]), {"highway": "primary"})
    assert bc.find_underpass([d], [osm, approach]) is None
    under = Feature("road", "osm:w9", LineString([ll(100, -100), ll(100, 100)]), {"highway": "primary"})
    assert bc.find_underpass([d], [osm, approach, under])["road"] == "osm:w9"
