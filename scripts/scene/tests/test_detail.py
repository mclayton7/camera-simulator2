"""REALISM R5 detail polygons: loading, normalisation, regions, scopes, clipping, the sample mask."""

from __future__ import annotations

import json

import numpy as np
import pytest
import shapely

from camsim_scene import detail as D
from camsim_scene.config import ConfigError
from camsim_scene.sources.base import Asset
from camsim_scene.tiling import Region

BBOX = (10.0, 10.0, 10.5, 10.5)
SQUARE = [[10.1, 10.1], [10.2, 10.1], [10.2, 10.2], [10.1, 10.2], [10.1, 10.1]]


def fc(*features, crs=None) -> dict:
    d = {"type": "FeatureCollection", "features": list(features)}
    if crs:
        d["crs"] = {"type": "name", "properties": {"name": crs}}
    return d


def feat(name, coords, kind="Polygon") -> dict:
    return {"type": "Feature", "properties": {"name": name}, "geometry": {"type": kind, "coordinates": coords}}


def write(tmp_path, doc) -> object:
    p = tmp_path / "detail.geojson"
    p.write_text(json.dumps(doc))
    return p


def test_loads_sorted_and_normalised(tmp_path):
    p = write(tmp_path, fc(feat("b", [SQUARE]), feat("a", [SQUARE[::-1]])))
    polys = D.load_polygons(p, BBOX)
    assert [x["name"] for x in polys] == ["a", "b"]
    assert polys[0]["wkt"] == polys[1]["wkt"]  # orientation normalised


def test_hash_ignores_order_orientation_and_noise(tmp_path):
    a = D.load_polygons(write(tmp_path, fc(feat("x", [SQUARE]), feat("y", [SQUARE]))), BBOX)
    noisy = [[x + 2e-9, y - 2e-9] for x, y in SQUARE[::-1]]
    noisy[-1] = noisy[0]
    b = D.load_polygons(write(tmp_path, fc(feat("y", [noisy]), feat("x", [SQUARE]))), BBOX)
    assert D.polygons_sha256(a) == D.polygons_sha256(b)


@pytest.mark.parametrize(
    "doc, msg",
    [
        ({"type": "Feature"}, "FeatureCollection"),
        (fc(), "FeatureCollection"),
        (fc(feat("Bad Name", [SQUARE])), "properties.name"),
        (fc(feat("a", [SQUARE]), feat("a", [SQUARE])), "duplicate"),
        (fc(feat("a", [[10.1, 10.1], [10.2, 10.1]], "LineString")), "Polygon or MultiPolygon"),
        (fc(feat("a", [[[10.1, 10.1], [10.2, 10.2], [10.2, 10.1], [10.1, 10.2], [10.1, 10.1]]])), "invalid"),
        (fc(feat("a", [[[9.9, 10.1], [10.2, 10.1], [10.2, 10.2], [9.9, 10.2], [9.9, 10.1]]])), "inside the bbox"),
        (fc(feat("a", [SQUARE]), crs="urn:ogc:def:crs:EPSG::3857"), "EPSG:4326"),
    ],
)
def test_rejects(tmp_path, doc, msg):
    with pytest.raises(ConfigError, match=msg):
        D.load_polygons(write(tmp_path, doc), BBOX)


def test_missing_file_is_a_config_error(tmp_path):
    with pytest.raises(ConfigError, match="detail.file"):
        D.load_polygons(tmp_path / "nope.geojson", BBOX)


def test_polygon_on_the_bbox_edge_is_accepted(tmp_path):
    edge = [[10.0, 10.0], [10.5, 10.0], [10.5, 10.1], [10.0, 10.1], [10.0, 10.0]]
    assert D.load_polygons(write(tmp_path, fc(feat("a", [edge]), crs="urn:ogc:def:crs:OGC:1.3:CRS84")), BBOX)


def test_regions_per_part_with_margin(tmp_path):
    other = [[10.3, 10.3], [10.4, 10.3], [10.4, 10.4], [10.3, 10.4], [10.3, 10.3]]
    polys = D.load_polygons(write(tmp_path, fc(feat("m", [[SQUARE], [other]], "MultiPolygon"))), BBOX)
    rs = D.regions(polys, {"terrain": 16, "imagery": 17}, {"terrain": 10, "imagery": 17}, 3.0)
    assert [r.name for r in rs] == ["detail:m/0", "detail:m/1", "detail_margin:m/0", "detail_margin:m/1"]
    assert rs[0].max_zoom == {"terrain": 16, "imagery": 17} and rs[2].max_zoom == {"terrain": 10, "imagery": 17}
    inner, outer = rs[0].geometry(), rs[2].geometry()
    assert outer.contains(inner)
    # 3 km of latitude ~ 0.02695 degrees
    assert outer.bounds[3] - inner.bounds[3] == pytest.approx(3.0 / 111.32, abs=1e-6)


def test_single_part_names_have_no_index(tmp_path):
    polys = D.load_polygons(write(tmp_path, fc(feat("p", [SQUARE]))), BBOX)
    assert [r.name for r in D.regions(polys, {"terrain": 1}, {"terrain": 1}, 0.0)] == ["detail:p", "detail_margin:p"]


def test_scope_geometry_and_scoped_map():
    rs = [
        Region.from_bounds("bbox", BBOX, {}),
        Region.from_bounds("detail:a", (10.1, 10.1, 10.2, 10.2), {}),
        Region.from_bounds("detail:b", (10.3, 10.3, 10.4, 10.4), {}),
        Region.from_bounds("detail_margin:a", (10.0, 10.0, 10.3, 10.3), {}),
    ]
    assert D.scope_geometry(rs, D.DETAIL).area == pytest.approx(0.02)
    assert D.scope_geometry(rs, D.MARGIN).area == pytest.approx(0.09)
    assert D.scope_geometry(rs[:1], D.DETAIL) is None
    layers = {"terrain": {"scoped": {"dep": "detail"}}, "imagery": {"scoped": {"naip": "detail_margin"}}}
    assert D.scoped_map(layers) == {"dep": "detail", "naip": "detail_margin"}
    assert D.scoped_map({"terrain": {}, "imagery": {}}) == {}


def test_clip_is_polygonal_or_none():
    scope = shapely.box(0, 0, 1, 1)
    assert D.clip(shapely.box(0.5, 0.5, 2, 2), scope).equals(shapely.box(0.5, 0.5, 1, 1))
    assert D.clip(shapely.box(1, 0, 2, 1), scope) is None  # touches along an edge only
    assert D.clip(shapely.box(3, 3, 4, 4), scope) is None


def test_discover_in_queries_each_part_and_merges():
    seen = []

    class Src:
        def discover(self, area):
            seen.append(tuple(area.bounds))
            return [
                Asset("near", "u1", metadata={"bbox": [0.5, 0.5, 1.5, 1.5]}),
                Asset("far", "u2", metadata={"bbox": [5.0, 5.0, 6.0, 6.0]}),
                Asset("nobbox", "u3"),
            ]

    scope = shapely.union_all([shapely.box(0, 0, 1, 1), shapely.box(2, 2, 3, 3)])
    got = D.discover_in(Src(), scope)
    assert [a.id for a in got] == ["near", "nobbox"]
    assert sorted(seen) == [(0.0, 0.0, 1.0, 1.0), (2.0, 2.0, 3.0, 3.0)]


def test_inside_mask():
    scope = shapely.box(0, 0, 1, 1)
    lon = np.array([[0.5, 1.5], [1.0, -0.1]])
    lat = np.array([[0.5, 0.5], [0.5, 0.5]])
    assert D.inside(scope, lon, lat).tolist() == [[True, False], [True, False]]


def test_discover_in_merges_ranks_consistently_across_parts():
    """Adapter order newest first: B > Y > A. Part 1 sees [Y, A], part 2 sees [B, Y'] (another tile of project Y).
    Per-call ranks would give B and Y both rank 0 (a tie broken by name); the merged ranks must keep B < Y < A."""
    calls = {
        (0.0, 0.0, 1.0, 1.0): [("y1", "Y"), ("a1", "A")],
        (2.0, 2.0, 3.0, 3.0): [("b1", "B"), ("y2", "Y")],
    }

    class Src:
        version = ""

        def discover(self, area):
            out = [Asset(i, i, group=g, rank=r) for r, (i, g) in enumerate(calls[tuple(area.bounds)])]
            self.version = {"y1": "2025-06-01", "b1": "2024-01-01"}[out[0].id]  # the newest comes first
            return out

    src = Src()
    scope = shapely.union_all([shapely.box(0, 0, 1, 1), shapely.box(2, 2, 3, 3)])
    got = {a.id: a.rank for a in D.discover_in(src, scope)}
    assert got["b1"] < got["y1"] < got["a1"] and got["b1"] < got["y2"]
    assert sorted(got.values()) == [0, 1, 2, 3]
    group_rank = {}
    for a in D.discover_in(src, scope):
        group_rank[a.group] = min(group_rank.get(a.group, a.rank), a.rank)
    assert group_rank["B"] < group_rank["Y"] < group_rank["A"]
    assert src.version == "2025-06-01"  # the newest over all calls, not the last call's (2024-01-01)


def test_discover_in_single_part_keeps_the_adapters_ranks():
    class Src:
        def discover(self, area):
            return [Asset(i, i, group=g, rank=r) for r, (i, g) in enumerate([("p1", "P"), ("p2", "P"), ("q1", "Q")])]

    got = [(a.id, a.rank) for a in D.discover_in(Src(), shapely.box(0, 0, 1, 1))]
    assert got == [("p1", 0), ("p2", 1), ("q1", 2)]
