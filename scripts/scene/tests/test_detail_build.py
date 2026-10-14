"""REALISM R5 end to end on the synthetic scene: a preview box (zoom 8) with one detail polygon (zoom 11)."""

from __future__ import annotations

import io
import json

import numpy as np
import pytest
import shapely
from fake_sources import synthetic_scene
from fakes import FakeHttp
from PIL import Image

from camsim_scene import detail as D
from camsim_scene import qmesh
from camsim_scene.cache import Cache
from camsim_scene.config import parse_scene
from camsim_scene.context import coverage
from camsim_scene.manifest import Manifest
from camsim_scene.pipeline import build_scene, fetch_scene, plan_scene
from camsim_scene.tiling import plan_tiles, tile_bounds

# z9 tiles are 0.3515625 deg: x540 = [9.84375, 10.1953125], x541 = [10.1953125, 10.546875]; y likewise (from 10.0
# lat: y284 = [9.84375, 10.1953125]). The polygon + 3 km (0.027 deg) stays west/south of 10.1953125.
POLY = [[10.0, 10.0], [10.15, 10.0], [10.15, 10.15], [10.0, 10.15], [10.0, 10.0]]
OUTSIDE_Z9 = [(541, 284), (540, 285), (541, 285)]


def detail_scene(tmp_path) -> dict:
    s = synthetic_scene(tmp_path / "src")
    p = tmp_path / "detail.geojson"
    feat = {"type": "Feature", "properties": {"name": "p"}, "geometry": {"type": "Polygon", "coordinates": [POLY]}}
    p.write_text(json.dumps({"type": "FeatureCollection", "features": [feat]}))
    s["profile"] = "preview"
    s["detail"] = {"file": str(p), "scoped": {"terrain": ["hi_dem"], "imagery": ["hi_rgb"]}}
    s["zoom"]["bbox"] = {"terrain": 8, "imagery": 8}
    s["zoom"]["detail"] = {"terrain": 11, "imagery": 11}
    s["sources"]["hi_dem"]["max_zoom"] = 12
    s["sources"]["hi_rgb"]["max_zoom"] = 12
    return s


def planned(tmp_path):
    pkg = tmp_path / "pkg"
    plan_scene(parse_scene(detail_scene(tmp_path)), pkg, http=FakeHttp({}), out=io.StringIO())
    return pkg


def test_fetch_clips_scoped_footprints(tmp_path):
    pkg = planned(tmp_path)
    m = fetch_scene(pkg, Cache(tmp_path / "cache"))
    sc = D.scopes(m)
    for sid in ("hi_dem", "hi_rgb"):
        fp = shapely.from_wkt(m.source(sid).assets[0].metadata["footprint"])
        assert sc[sid].buffer(1e-6).covers(fp)
    assert shapely.from_wkt(m.source("hi_dem").assets[0].metadata["footprint"]).bounds[2] == pytest.approx(10.15)


def test_coverage_before_fetch_is_clipped(tmp_path):
    m = Manifest.load(planned(tmp_path) / "manifest.json")
    cov = coverage(m, "terrain", use_bbox=True)
    hi = [g for g, lim in zip(cov.geoms, cov.limits) if lim == 12]
    assert len(hi) == 1 and hi[0].bounds == pytest.approx((10.0, 10.0, 10.15, 10.15))


def test_deep_tiles_only_inside_the_scopes(tmp_path):
    m = fetch_scene(planned(tmp_path), Cache(tmp_path / "cache"))
    detail_g = D.scope_geometry(m.region_objs(), D.DETAIL)
    margin_g = D.scope_geometry(m.region_objs(), D.MARGIN)
    for layer, scope in (("terrain", detail_g), ("imagery", margin_g)):
        p = plan_tiles(m.region_objs(), layer, coverage(m, layer))
        assert p.max_zoom == 11
        for z in range(9, 12):
            for k in p.tiles.get(z, []):
                x, y = int(k) >> 32, int(k) & 0xFFFFFFFF
                parent_box = shapely.box(*tile_bounds(z - 1, x // 2, y // 2))
                assert parent_box.intersects(scope), (layer, z, x, y)
