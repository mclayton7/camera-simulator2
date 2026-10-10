"""OpenStreetMap through Geofabrik extracts (ODbL 1.0; REALISM R2). Discovery picks the smallest extract whose polygon
contains the area (or the configured `extract`) and pins the dated daily file; the cache stores it by sha256, which is
the snapshot. read_osm streams the PBF with pyosmium and returns the roads, paved areas and rail the burn needs.

Geofabrik layout (checked 2026-10-10): index-v1.json is a GeoJSON FeatureCollection; each feature has
properties.id and properties.urls.pbf = ".../<extract>-latest.osm.pbf" (socal: north-america/us/california/
socal-latest.osm.pbf). Dated daily files sit beside it as "<extract>-<yymmdd>.osm.pbf" (socal-261008 returned 200;
the current and previous day returned 404 until that day's build lands, so pin a date at least a couple of days old).
"""

from __future__ import annotations

import datetime as dt
import re
from pathlib import Path

import shapely
from shapely.geometry import box, shape

from ..features import Feature
from .base import Area, Asset, Layer, SourceBase

INDEX_URL = "https://download.geofabrik.de/index-v1.json"
# pyosmium's WKBFactory returns hex strings; shapely.from_wkb reads them directly.
_GEOM_ERRORS = (RuntimeError, ValueError, shapely.errors.ShapelyError)
AREA_TAGS = (("amenity", "parking"), ("aeroway", "apron"))


def dated_url(latest_url: str, date: str) -> str:
    d = dt.date.fromisoformat(date)
    return re.sub(r"-latest\.osm\.pbf$", f"-{d:%y%m%d}.osm.pbf", latest_url)


class OsmGeofabrik(SourceBase):
    id = "osm_geofabrik"
    layer = Layer.FEATURES
    licence = "ODbL-1.0"
    dataset = "OpenStreetMap (Geofabrik extract)"
    attribution = "© OpenStreetMap contributors"
    area_kind = "margin"

    def discover(self, area: Area) -> list[Asset]:
        idx = self.http.get_json(INDEX_URL, {})
        want = box(*area.bounds)
        forced = self._options.get("extract")
        cands = []
        for f in idx["features"]:
            p = f["properties"]
            if forced and p["id"] != forced:
                continue
            if f.get("geometry") is None or "pbf" not in p.get("urls", {}):
                continue
            g = shape(f["geometry"])
            if g.contains(want):
                cands.append((g.area, p["id"], p["urls"]["pbf"]))
        if not cands:
            if forced:
                raise ValueError(f"Geofabrik extract {forced!r} does not contain {area.bounds}")
            raise ValueError(f"no Geofabrik extract contains {area.bounds}")
        _, ext, url = min(cands)
        date = self._options.get("date") or dt.datetime.now(dt.UTC).date().isoformat()
        self.version = date
        return [
            Asset(
                id=f"{ext}-{date}",
                url=dated_url(url, date),
                group=self.id,
                metadata={"extract": ext, "date": date},
            )
        ]

    def prepare(self, path, asset, cache):
        return None


def _kind(tags) -> str | None:
    if "highway" in tags and tags.get("area") != "yes":
        return "road"
    if "railway" in tags:
        return "rail"
    if tags.get("aeroway") in ("runway", "taxiway"):
        return "road"
    return None


def _area_kind(tags) -> str | None:
    if "area:highway" in tags or ("highway" in tags and tags.get("area") == "yes"):
        return "paved_area"
    if any(tags.get(k) == v for k, v in AREA_TAGS):
        return "paved_area"
    return None


def read_osm(pbf: Path, bounds, settings: dict) -> list[Feature]:
    """Roads / rail ways and paved-area polygons whose geometry intersects bounds. Tags are copied as plain dicts; the
    width / surface / skip rules are applied by features.py, not here."""
    import osmium

    want = box(*bounds)
    wkb = osmium.geom.WKBFactory()
    keep_roads, keep_rail, keep_areas = settings["roads"], settings["rail"], settings["paved_areas"]
    classes = set(settings["road_classes"]) | {c + "_link" for c in settings["road_classes"]}
    out: list[Feature] = []
    fp = osmium.FileProcessor(str(pbf)).with_locations().with_areas()
    for o in fp:
        if not (o.is_way() or o.is_area()):
            continue
        tags = {t.k: t.v for t in o.tags}
        if o.is_way():
            k = _kind(tags)
            if k is None or (k == "road" and not keep_roads) or (k == "rail" and not keep_rail):
                continue
            if k == "road" and "highway" in tags and tags["highway"] not in classes:
                continue
            if k == "rail" and tags.get("railway") not in ("rail", "light_rail"):
                continue
            try:
                g = shapely.from_wkb(wkb.create_linestring(o))
            except _GEOM_ERRORS:
                continue  # a way with missing node locations (cut at the extract edge)
            if g.intersects(want):
                out.append(Feature(k, f"osm:w{o.id}", g, tags))
        elif keep_areas:
            if _area_kind(tags) is None or tags.get("parking") in ("underground", "multi-storey"):
                continue
            try:
                g = shapely.from_wkb(wkb.create_multipolygon(o))
            except _GEOM_ERRORS:
                continue
            if g.intersects(want):
                out.append(Feature("paved_area", f"osm:a{o.id}", g, tags))
    out.sort(key=lambda f: f.source_id)
    return out
