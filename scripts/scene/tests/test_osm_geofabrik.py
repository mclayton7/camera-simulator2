import datetime as dt
from pathlib import Path

import osmium
import pytest
from fakes import FakeHttp, fixture_json

from camsim_scene.features import Feature
from camsim_scene.sources import make_source
from camsim_scene.sources.base import Area
from camsim_scene.sources import osm_geofabrik
from camsim_scene.sources.osm_geofabrik import INDEX_URL, dated_url, read_osm

SETTINGS = {"roads": True, "paved_areas": True, "rail": True, "road_classes": ["motorway", "residential"]}


SOCAL = "https://download.geofabrik.de/north-america/us/california/socal-%s.osm.pbf"
AREA = Area((-117.6, 33.2, -117.3, 33.5))


def _http(*days):
    routes = {INDEX_URL: fixture_json("geofabrik-index.json")}
    routes.update({("HEAD", SOCAL % d): (200, 1234) for d in days})
    return FakeHttp(routes)


def test_smallest_extract_containing_the_area_wins():
    http = _http("261009")
    src = make_source("osm_geofabrik", options={"date": "2026-10-09"}, http=http)
    [a] = src.discover(Area((-117.6, 33.2, -117.3, 33.5)))
    assert a.metadata["extract"] == "socal"
    assert a.url == "https://download.geofabrik.de/north-america/us/california/socal-261009.osm.pbf"


def test_forced_extract_must_contain_the_area():
    http = _http("261009")
    src = make_source("osm_geofabrik", options={"extract": "norcal", "date": "2026-10-09"}, http=http)
    with pytest.raises(ValueError, match="does not contain"):
        src.discover(Area((-117.6, 33.2, -117.3, 33.5)))


def test_unset_date_pins_the_newest_existing_file(monkeypatch):
    monkeypatch.setattr(osm_geofabrik, "today_utc", lambda: dt.date(2026, 10, 10))
    src = make_source("osm_geofabrik", http=_http("261008", "261005"))
    [a] = src.discover(AREA)
    assert (a.id, a.metadata["date"], a.size, src.version) == ("socal-261008", "2026-10-08", 1234, "2026-10-08")
    assert a.url == SOCAL % "261008"


def test_no_file_in_seven_days_is_an_error(monkeypatch):
    monkeypatch.setattr(osm_geofabrik, "today_utc", lambda: dt.date(2026, 10, 10))
    src = make_source("osm_geofabrik", http=_http("261002"))  # eight days back
    with pytest.raises(ValueError, match="socal.*last 7 days"):
        src.discover(AREA)


def test_configured_date_must_exist():
    src = make_source("osm_geofabrik", options={"date": "2026-10-09"}, http=_http())
    with pytest.raises(ValueError, match="socal.*2026-10-09"):
        src.discover(AREA)


def test_dated_url():
    assert dated_url("https://x/a/socal-latest.osm.pbf", "2026-01-02") == "https://x/a/socal-260102.osm.pbf"


def _write_pbf(path: Path):
    w = osmium.SimpleWriter(str(path))
    pts = {1: (-117.50, 33.30), 2: (-117.49, 33.30), 3: (-117.48, 33.30), 4: (-117.40, 34.50), 5: (-117.39, 34.50)}
    for nid, (lon, lat) in pts.items():
        w.add_node(osmium.osm.mutable.Node(id=nid, location=(lon, lat)))
    w.add_way(osmium.osm.mutable.Way(id=10, nodes=[1, 2], tags={"highway": "motorway", "lanes": "3"}))
    w.add_way(osmium.osm.mutable.Way(id=11, nodes=[2, 3], tags={"highway": "motorway", "bridge": "yes"}))
    w.add_way(osmium.osm.mutable.Way(id=12, nodes=[1, 3], tags={"highway": "footway"}))
    w.add_way(osmium.osm.mutable.Way(id=13, nodes=[4, 5], tags={"highway": "motorway"}))  # outside the area
    w.add_way(osmium.osm.mutable.Way(id=14, nodes=[1, 3], tags={"railway": "rail"}))
    w.close()


def test_read_osm_keeps_wanted_ways_in_area(tmp_path):
    pbf = tmp_path / "t.osm.pbf"
    _write_pbf(pbf)
    feats = read_osm(pbf, (-117.6, 33.2, -117.3, 33.5), SETTINGS)
    ids = sorted((f.kind, f.source_id) for f in feats)
    assert ids == [("rail", "osm:w14"), ("road", "osm:w10"), ("road", "osm:w11")]  # the bridge is dropped later
    assert all(isinstance(f, Feature) for f in feats)


def test_read_osm_paved_areas(tmp_path):
    pbf = tmp_path / "a.osm.pbf"
    w = osmium.SimpleWriter(str(pbf))
    sq = [(-117.50, 33.30), (-117.49, 33.30), (-117.49, 33.31), (-117.50, 33.31)]
    nid = 100
    rings = {}
    for way in (20, 21, 22):
        ids = []
        for dx, (lon, lat) in zip(range(4), sq):
            nid += 1
            w.add_node(osmium.osm.mutable.Node(id=nid, location=(lon + (way - 20) * 0.02, lat)))
            ids.append(nid)
        rings[way] = ids + [ids[0]]
    w.add_way(osmium.osm.mutable.Way(id=20, nodes=rings[20], tags={"amenity": "parking"}))
    w.add_way(osmium.osm.mutable.Way(id=21, nodes=rings[21], tags={"amenity": "parking", "parking": "underground"}))
    w.add_way(osmium.osm.mutable.Way(id=22, nodes=rings[22], tags={"highway": "pedestrian", "area": "yes"}))
    w.close()
    feats = read_osm(pbf, (-117.6, 33.2, -117.3, 33.5), SETTINGS)
    # way ids, not pyosmium's 2*id area ids; the pedestrian area is not a road
    assert sorted((f.kind, f.source_id) for f in feats) == [("paved_area", "osm:w20"), ("paved_area", "osm:w22")]
    assert all(f.geom.geom_type == "MultiPolygon" or f.geom.geom_type == "Polygon" for f in feats)
