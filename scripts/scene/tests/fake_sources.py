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
            decode=o.get("decode"),
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


def tone_truth(raw):
    """The synthetic 'true' Sentinel-2 -> NAIP map (deliberately not today's decoder)."""
    return 255.0 * (np.asarray(raw, np.float64) / 4000.0) ** (1.0 / 2.2) - 30.0


def raw_truth(lon, lat, band: int):
    """Smooth synthetic Sentinel-2 raw DN (period ~1.7 km): bilinear sampling stays within ~7.5 raw DN of it (under
    1 NAIP DN)."""
    return 1900.0 + 1500.0 * np.sin(np.asarray(lon) * 400.0 + band) * np.cos(np.asarray(lat) * 300.0)


def _cell_centres(box, res):
    w, s, e, n = box
    nx, ny = round((e - w) / res), round((n - s) / res)
    return np.meshgrid(w + (np.arange(nx) + 0.5) * res, n - (np.arange(ny) + 0.5) * res)


def naip_s2_scene(
    root: Path,
    offset=lambda lon, lat: 0.0,
    glint_east: bool = False,
    ref_box=(10.0, 10.0, 10.2, 10.2),
    s2_box=(9.8, 9.8, 10.4, 10.4),
    res=0.0005,
) -> dict:
    """NAIP (`naip_pc`, uint8 = tone_truth(raw) + offset) over ref_box, Sentinel-2 (`wc_s2`, raw uint16 DN) over
    s2_box, global base RGB + DEM. glint_east: NAIP east of the ref box's middle is +60 DN (and a class raster,
    classes.tif, marks it water: 80; west 10)."""
    base = synthetic_scene(root / "base")
    root.mkdir(parents=True, exist_ok=True)
    lon, lat = _cell_centres(s2_box, res)
    raw = np.stack([raw_truth(lon, lat, b) for b in range(3)]).round().astype(np.uint16)
    write_geotiff(root / "s2.tif", raw, s2_box[0], s2_box[3], res, overviews=(2, 4))
    lon, lat = _cell_centres(ref_box, res)
    rawr = np.stack([raw_truth(lon, lat, b) for b in range(3)]).round()
    ref = tone_truth(rawr) + offset(lon, lat)
    mid = (ref_box[0] + ref_box[2]) / 2
    if glint_east:
        ref = np.where(lon >= mid, ref + 60.0, ref)
    write_geotiff(
        root / "naip.tif", np.clip(np.rint(ref), 1, 255).astype(np.uint8), ref_box[0], ref_box[3], res, overviews=(2, 4)
    )
    classes = np.where(lon >= mid, 80, 10).astype(np.uint8)
    write_geotiff(root / "classes.tif", classes, ref_box[0], ref_box[3], res, overviews=(2, 4))
    s = base["sources"]
    return {
        "name": "edge",
        "bbox": list(ref_box),
        "ring_km": 30,
        "imagery_margin_km": 0,
        "priorities": {"terrain": ["base_dem"], "imagery": ["naip_pc", "wc_s2", "base_rgb"], "landcover": []},
        "zoom": {
            "globe": {"terrain": 2, "imagery": 2},
            "ring": {"terrain": 3, "imagery": 3},
            "bbox": {"terrain": 3, "imagery": 9},
        },
        "sources": {
            "base_dem": s["base_dem"],
            "base_rgb": s["base_rgb"],
            "naip_pc": source(
                "imagery", root / "naip.tif", ref_box, max_zoom=15, bands=[1, 2, 3], nodata_rule="all_zero"
            ),
            "wc_s2": source(
                "imagery",
                root / "s2.tif",
                s2_box,
                max_zoom=13,
                bands=[1, 2, 3],
                nodata_rule="all_zero",
                decode="s2_reflectance",
            ),
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


COAST = 10.10  # coast_scene's shoreline: land west of it, sea east
LAKE = (10.0700, 10.0915, 10.0727, 10.0942)  # a 300 m lake inside the NAIP box (W S E N)
POND = (10.0450, 10.0740, 10.0477, 10.0767)  # a 300 m pond outside the NAIP box, inside Sentinel-2's


def coast_scene(root: Path, shore: float = COAST, extra_water=None) -> dict:
    """naip_s2_scene at 11 m with a shoreline: NAIP over (10.06, 10.08, 10.14, 10.12), 60 DN darker east of COAST
    (sea fill), Sentinel-2 over (10.04, 10.07, 10.16, 10.13), and a land-cover source `worldcover` (worldcover.tif
    over the Sentinel-2 box, no overviews): 10 west of `shore`, 80 (water) east of it, in LAKE, and in `extra_water`
    (a W S E N box) when given."""
    box = (10.04, 10.07, 10.16, 10.13)
    scene = naip_s2_scene(
        root,
        offset=lambda lon, lat: np.where(np.asarray(lon) >= COAST, -60.0, 0.0),
        ref_box=(10.06, 10.08, 10.14, 10.12),
        s2_box=box,
        res=0.0001,
    )
    lon, lat = _cell_centres(box, 0.0001)
    codes = np.where(lon >= shore, 80, 10).astype(np.uint8)
    for w, s, e, n in [LAKE] + ([extra_water] if extra_water else []):
        codes[(lon >= w) & (lon < e) & (lat >= s) & (lat < n)] = 80
    write_geotiff(root / "worldcover.tif", codes, box[0], box[3], 0.0001, overviews=())
    scene["priorities"] = {**scene["priorities"], "landcover": ["worldcover"]}
    scene["sources"]["worldcover"] = source("landcover", root / "worldcover.tif", box)
    return scene
