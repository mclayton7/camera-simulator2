"""USGS 3DEP 1 m project DEMs (10 x 10 km UTM tiles, NAD83 + NAVD88 with the project's geoid). Projects are
groups, newest collection first; the geoid comes from the WESM index (or the `geoid_overrides` option,
{project: "GEOID18"}). A project whose geoid is unknown or has no vendored PROJ grid is skipped."""

from __future__ import annotations

import datetime as dt
import logging
import re
from collections import defaultdict
from pathlib import Path

from ..datum import geoid_datum
from ..net import HttpError
from .base import Area, Asset, Layer, SourceBase, SourceRaster
from .tnm import tnm_products

log = logging.getLogger(__name__)
DATASET = "Digital Elevation Model (DEM) 1 meter"
WESM_QUERY = "https://index.nationalmap.gov/arcgis/rest/services/3DEPElevationIndex/MapServer/24/query"
URL_RE = re.compile(r"/1m/Projects/([^/]+)/TIFF/(USGS_1M_(\d+)_x\d+y\d+_[^/]+)\.tif$")


class Dep31m(SourceBase):
    id = "dep3_1m"
    layer = Layer.TERRAIN
    licence = "LicenseRef-PublicDomain-USGov"
    dataset = "USGS 3DEP 1 m project DEMs"
    attribution = "U.S. Geological Survey, 3D Elevation Program (3DEP)"
    max_zoom = 16
    area_kind = "bbox"

    def _wesm(self, bounds) -> dict[str, dict]:
        d = self.http.get_json(
            WESM_QUERY,
            {
                "where": "1=1",
                "geometry": ",".join(f"{v:.6f}" for v in bounds),
                "geometryType": "esriGeometryEnvelope",
                "inSR": "4326",
                "spatialRel": "esriSpatialRelIntersects",
                "outFields": "project,workunit,geoid,collect_end",
                "returnGeometry": "false",
                "f": "json",
            },
        )
        if "error" in d:
            raise HttpError(f"WESM query failed: {d['error']} (set sources.dep3_1m.geoid_overrides to proceed)")
        out: dict[str, dict] = {}
        for f in d.get("features", []):
            a = f["attributes"]
            for key in {a.get("project"), a.get("workunit")} - {None}:
                p = out.setdefault(key, {"geoids": set(), "collect_end": 0})
                if a.get("geoid"):
                    p["geoids"].add(a["geoid"])
                p["collect_end"] = max(p["collect_end"], int(a.get("collect_end") or 0))
        return out

    def discover(self, area: Area) -> list[Asset]:
        overrides = self._options.get("geoid_overrides", {})
        wesm = self._wesm(area.bounds)
        by_project: dict[str, list] = defaultdict(list)
        for it in tnm_products(self.http, area.bounds, DATASET):
            m = URL_RE.search(it.get("downloadURL", ""))
            if m:
                by_project[m.group(1)].append((m, it))

        def collected(p: str) -> str:
            ms = wesm.get(p, {}).get("collect_end", 0)
            if ms:
                return dt.datetime.fromtimestamp(ms / 1000, dt.UTC).date().isoformat()
            return max(it.get("publicationDate", "") for _, it in by_project[p])

        assets: list[Asset] = []
        for p in sorted(by_project, key=lambda p: (collected(p), p), reverse=True):
            geoids = wesm.get(p, {}).get("geoids", set())
            geoid = overrides.get(p) or (next(iter(geoids)) if len(geoids) == 1 else None)
            did = geoid_datum(geoid) if geoid else None
            if did is None:
                log.warning(
                    "dep3_1m: skipping project %s: geoid %s is unknown or has no PROJ grid (%s)",
                    p,
                    geoid or sorted(geoids),
                    "set sources.dep3_1m.geoid_overrides if you know it",
                )
                continue
            for m, it in sorted(by_project[p], key=lambda t: t[0].group(2)):
                bb = it["boundingBox"]
                assets.append(
                    Asset(
                        id=m.group(2),
                        url=it["downloadURL"],
                        size=it.get("sizeInBytes"),
                        group=p,
                        rank=len(assets),
                        metadata={
                            "project": p,
                            "geoid": geoid,
                            "datum": did,
                            "utm_zone": int(m.group(3)),
                            "collected": collected(p),
                            "bbox": [bb["minX"], bb["minY"], bb["maxX"], bb["maxY"]],
                        },
                    )
                )
        self.version = max((it.get("publicationDate", "") for v in by_project.values() for _, it in v), default="")
        return assets

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return SourceRaster(path=Path(path), datum=asset.metadata["datum"], nodata=-999999.0)
