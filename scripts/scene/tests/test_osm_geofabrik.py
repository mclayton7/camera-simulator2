from pathlib import Path

import osmium
import pytest
from fakes import FakeHttp, fixture_json

from camsim_scene.features import Feature
from camsim_scene.sources import make_source
from camsim_scene.sources.base import Area
from camsim_scene.sources.osm_geofabrik import INDEX_URL, dated_url, read_osm

SETTINGS = {"roads": True, "paved_areas": True, "rail": True, "road_classes": ["motorway", "residential"]}


def test_smallest_extract_containing_the_area_wins():
    http = FakeHttp({INDEX_URL: fixture_json("geofabrik-index.json")})
    src = make_source("osm_geofabrik", options={"date": "2026-10-09"}, http=http)
    [a] = src.discover(Area((-117.6, 33.2, -117.3, 33.5)))
    assert a.metadata["extract"] == "socal"
    assert a.url == "https://download.geofabrik.de/north-america/us/california/socal-261009.osm.pbf"


def test_forced_extract_must_contain_the_area():
    http = FakeHttp({INDEX_URL: fixture_json("geofabrik-index.json")})
    src = make_source("osm_geofabrik", options={"extract": "norcal", "date": "2026-10-09"}, http=http)
    with pytest.raises(ValueError, match="does not contain"):
        src.discover(Area((-117.6, 33.2, -117.3, 33.5)))


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
