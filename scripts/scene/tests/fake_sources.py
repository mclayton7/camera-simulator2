"""Synthetic sources for context/layer/pipeline tests: small local GeoTIFFs served as file:// URLs.
Referenced from scene data as adapter "fake_sources:FakeSource" (importable in spawned workers too)."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import shapely
from rasters import write_geotiff

from camsim_scene import datum, fsutil
from camsim_scene.config import TILING, layer_settings, parse_scene
from camsim_scene.context import BuildContext
from camsim_scene.manifest import AssetRecord, Manifest, SourceRecord
from camsim_scene.sources import make_source
from camsim_scene.sources.base import Asset, Layer, SourceBase, SourceRaster

ADAPTER = "fake_sources:FakeSource"


class FakeSource(SourceBase):
    def __init__(self, options=None, http=None):
        super().__init__(options, http)
        o = self._options
        self.layer = Layer(o["layer"])
        self.max_zoom = int(o["max_zoom"])
        self.global_coverage = bool(o.get("global", False))
        self.datum = o.get("datum", "wgs84")
        self.licence = o.get("licence", "LicenseRef-PublicDomain-USGov")
        self.dataset, self.version, self.attribution, self.area_kind = (
            f"fake {o['layer']}",
            "1",
            "synthetic test data",
            "globe",
        )

    def discover(self, area) -> list[Asset]:
        out = [
            Asset(
                id=f["id"],
                url=Path(f["path"]).as_uri(),
                group=f.get("group", self.id),
                rank=i,
                metadata={"bbox": list(f["bbox"])},
            )
            for i, f in enumerate(self._options["files"])
        ]
        if "geoid" in self._options:
            out.append(
                Asset(
                    id="geoid",
                    url=Path(self._options["geoid"]).as_uri(),
                    group=self.id,
                    rank=len(out),
                    role="geoid",
                    metadata={"bbox": [-180, -90, 180, 90]},
                )
            )
        return out

    def prepare(self, path, asset, cache):
        return None

    def open(self, path, asset) -> SourceRaster:
        o = self._options
        return SourceRaster(
            path=Path(path),
            datum=self.datum,
            bands=tuple(o.get("bands", [1])),
            nodata=o.get("nodata"),
            nodata_rule=o.get("nodata_rule", "value"),
            clamp_edges=self.global_coverage,
            vertical_asset=f"{self.id}/geoid" if "geoid" in o else None,
        )


def source(layer, path, bbox, max_zoom=8, **kw) -> dict:
    return {
        "adapter": ADAPTER,
        "layer": layer,
        "max_zoom": max_zoom,
        "files": [{"id": Path(path).stem, "path": str(path), "bbox": list(bbox)}],
        **kw,
    }


def synthetic_scene(root: Path, bbox=(10.0, 10.0, 10.5, 10.5)) -> dict:
    """A global 1-degree DEM + RGB, and a 0.001-degree DEM + RGB around bbox (north half of the RGB brighter)."""
    root.mkdir(parents=True, exist_ok=True)
    lon, lat = np.arange(360) - 179.5, 89.5 - np.arange(180)
    dem = (100.0 + 50.0 * np.sin(np.radians(lat))[:, None] * np.cos(np.radians(lon))[None, :]).astype(np.float32)
    write_geotiff(root / "base_dem.tif", dem, -180.0, 90.0, 1.0)
    rgb = np.zeros((3, 180, 360), np.uint8)
    rgb[0], rgb[1], rgb[2] = 40, 80, 160
    write_geotiff(root / "base_rgb.tif", rgb, -180.0, 90.0, 1.0)
    w, s, e, n = bbox
    pad, res = 0.1, 0.001
    nx, ny = round((e - w + 2 * pad) / res), round((n - s + 2 * pad) / res)
    yy, xx = np.mgrid[0:ny, 0:nx]
    write_geotiff(
        root / "hi_dem.tif",
        (205.0 + 20.0 * np.sin(xx / 50.0) * np.cos(yy / 70.0)).astype(np.float32),
        w - pad,
        n + pad,
        res,
        nodata=-9999.0,
        overviews=(2, 4, 8),
    )
    hrgb = np.zeros((3, ny, nx), np.uint8)
    hrgb[0], hrgb[1], hrgb[2] = 200, (xx % 256).astype(np.uint8), 50
    hrgb[0, : ny // 2] = 250
    write_geotiff(root / "hi_rgb.tif", hrgb, w - pad, n + pad, res, overviews=(2, 4, 8))
    hi_bbox = [w - pad, n + pad - ny * res, w - pad + nx * res, n + pad]
    globe = [-180, -90, 180, 90]
    return {
        "name": "synthetic",
        "bbox": list(bbox),
        "ring_km": 30,
        "priorities": {"terrain": ["hi_dem", "base_dem"], "imagery": ["hi_rgb", "base_rgb"], "landcover": []},
        "zoom": {
            "globe": {"terrain": 2, "imagery": 2},
            "ring": {"terrain": 3, "imagery": 3},
            "bbox": {"terrain": 6, "imagery": 6},
        },
        "sources": {
            "base_dem": source("terrain", root / "base_dem.tif", globe, **{"global": True}),
            "hi_dem": source("terrain", root / "hi_dem.tif", hi_bbox, nodata=-9999.0),
            "base_rgb": source("imagery", root / "base_rgb.tif", globe, bands=[1, 2, 3], **{"global": True}),
            "hi_rgb": source("imagery", root / "hi_rgb.tif", hi_bbox, bands=[1, 2, 3], nodata_rule="all_zero"),
        },
    }


def fake_context(root: Path, scene: dict | None = None) -> BuildContext:
    """The context a fetched manifest would give, built directly from local files (no cache, no HTTP)."""
    scene = scene or synthetic_scene(root / "src")
    plan = parse_scene(scene)
    records, paths, datums = [], {}, set()
    for sid in plan.source_ids():
        opts = dict(plan.source_options[sid])
        adapter = opts.pop("adapter")
        src = make_source(sid, adapter, opts)
        assets = []
        for a in src.discover(None):
            path = Path(a.url.removeprefix("file://"))
            paths[f"{sid}/{a.id}"] = str(path)
            fp = None if src.global_coverage else shapely.to_wkt(shapely.box(*a.metadata["bbox"]), rounding_precision=7)
            assets.append(
                AssetRecord(
                    id=a.id,
                    url=a.url,
                    sha256=fsutil.sha256_file(path),
                    size=path.stat().st_size,
                    group=a.group,
                    rank=a.rank,
                    role=a.role,
                    metadata={**a.metadata, "footprint": fp},
                )
            )
        datums.add(src.datum)
        records.append(
            SourceRecord(sid, adapter, src.dataset, src.version, src.licence, src.attribution, src.options(), assets)
        )
    m = Manifest(
        name=plan.name,
        bbox=list(plan.bbox),
        seed=plan.seed,
        regions=[r.to_dict() for r in plan.regions()],
        tiling=TILING,
        layers=layer_settings(plan),
        datum=datum.manifest_section(datums),
        sources=records,
        licence_allow=["CC-BY-4.0", "LicenseRef-PublicDomain-USGov"],
        tool={"name": "camsim-scene", "version": "test"},
    )
    return BuildContext(str(root / "pkg"), m.to_dict(), paths, {})


def build_synthetic(tmp_path: Path, scene: dict | None = None, jobs: int = 1, name: str = "pkg", cache=None):
    """plan + fetch + build a synthetic package; returns (pkg, cache, build info)."""
    import io

    from fakes import FakeHttp

    from camsim_scene.cache import Cache
    from camsim_scene.pipeline import build_scene, plan_scene

    scene = scene or synthetic_scene(tmp_path / "src")
    cache = cache or Cache(tmp_path / "cache")
    pkg = tmp_path / name
    plan_scene(parse_scene(scene), pkg, http=FakeHttp({}), out=io.StringIO())
    return pkg, cache, build_scene(pkg, cache, jobs=jobs)
