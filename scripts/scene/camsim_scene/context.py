"""Per-process view of a fetched manifest: a SourceRaster and DatumTransform per data asset (and a (red, NIR) raster
for the NDVI layer), ordered by layer priority, behind an STRtree of footprints, plus the package's imagery colour
match (imagery/balance.json) when there is one, plus the package's NDVI fit (ndvi/fit.json) and water mask when it has
an NDVI layer. BuildContext (plain data) is what crosses the process boundary; each worker builds its WorkerState
once."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import shapely

from .balance import FILE as BALANCE_FILE
from .balance import Balance
from .datum import DatumTransform
from .fsutil import sha256_bytes
from .layers.imagery import ref_interior
from .manifest import AssetRecord, Manifest
from .ndvi_fit import FILE as NDVI_FIT_FILE
from .ndvi_fit import NdviFit
from .sources import make_source
from .sources.base import M_PER_DEG, Asset, SourceRaster
from .tiling import GLOBE, Bounds, Coverage
from .water import Water

GEOID_TARGET_M = 900.0  # ETOPO geoid grid resolution: read at full resolution, never finer


class ContextError(Exception):
    pass


@dataclass(frozen=True)
class BuildContext:
    pkg: str
    manifest: dict
    asset_paths: dict  # "<source>/<asset>" -> local file (prepared file when there is one)
    grid_paths: dict  # PROJ grid name -> local file


@dataclass
class Entry:
    qid: str
    sha256: str
    order: tuple  # (source priority, group min rank, group, asset rank, asset id); order[:3] = merge group
    limit: int
    raster: SourceRaster
    transform: DatumTransform


def lattice_spacing_deg(lon, lat) -> float:
    """Node spacing of a regular lattice of query points (offset_lattice: tile_size(z)/32, so it depends on the zoom
    alone); the larger of the two axes' median steps, 0 for a single point. Rounded so float noise can't change it."""
    steps = [0.0]
    for v in (lon, lat):
        d = np.diff(np.unique(np.round(np.asarray(v, np.float64).ravel(), 9)))
        if d.size:
            steps.append(float(np.median(d)))
    return round(max(steps), 9)


def geoid_target_m(lon, lat) -> float:
    """Geoid read resolution for a query: its lattice spacing, never finer than the grid. Coarse zooms then read an
    average overview instead of the full grid over their whole extent (memory bound: z0 would be ~7 GB)."""
    return max(GEOID_TARGET_M, M_PER_DEG * lattice_spacing_deg(lon, lat))


def to_asset(a: AssetRecord) -> Asset:
    return Asset(
        id=a.id,
        url=a.url,
        size=a.size,
        group=a.group,
        rank=a.rank,
        role=a.role,
        metadata=dict(a.metadata),
        sha256=a.sha256,
    )


def _footprint(a: AssetRecord, use_bbox: bool):
    if use_bbox:
        bb = a.metadata.get("bbox")
        return None if bb is None else shapely.box(*bb)
    if "footprint" not in a.metadata:
        raise ContextError(f"asset {a.id} has no footprint in manifest.json: run `camsim-scene fetch`")
    wkt = a.metadata["footprint"]
    return None if wkt is None else shapely.from_wkt(wkt)


def reference_footprints(m: Manifest, sid: str) -> list:
    """Footprints of a source's data assets (those that hold data): the colour match's reference coverage."""
    return [
        shapely.from_wkt(a.metadata["footprint"])
        for a in m.source(sid).assets
        if a.role == "data" and a.metadata.get("footprint")
    ]


def class_rasters(m: Manifest, asset_paths: dict) -> tuple[list, list[str]]:
    """The package's WorldCover rasters (sorted by asset id) and their sha256s: the fit's land mask and the leaves'
    water mask. Empty when the package has no WorldCover."""
    if "worldcover" not in m.layers["landcover"]["priorities"]:
        return [], []
    rec = m.source("worldcover")
    src = make_source(rec.id, rec.adapter, rec.options)
    data = sorted((a for a in rec.assets if a.role == "data"), key=lambda a: a.id)
    return [src.open(Path(asset_paths[f"{rec.id}/{a.id}"]), to_asset(a)) for a in data], [a.sha256 for a in data]


def coverage(m: Manifest, layer: str, use_bbox: bool = False) -> Coverage:
    """Footprints of a layer's data assets, each with its source's zoom limit (discovery bboxes before fetch)."""
    cov = Coverage()
    for sid in m.layers[layer]["priorities"]:
        rec = m.source(sid)
        src = make_source(rec.id, rec.adapter, rec.options)
        if src.global_coverage:
            cov.add(shapely.box(*GLOBE), src.max_zoom)
            continue
        for a in rec.assets:
            geom = _footprint(a, use_bbox) if a.role == "data" else None
            if geom is not None:
                cov.add(geom, src.max_zoom)
    return cov


class LayerIndex:
    def __init__(self, entries: list[Entry], geoms: list):
        self.entries = entries
        self.tree = shapely.STRtree(geoms) if geoms else None

    def query(self, bounds: Bounds) -> list[Entry]:
        if self.tree is None:
            return []
        idx = self.tree.query(shapely.box(*bounds), predicate="intersects")
        return sorted((self.entries[i] for i in idx), key=lambda e: e.order)


class WorkerState:
    def __init__(self, ctx: BuildContext):
        self.ctx = ctx
        self.pkg = Path(ctx.pkg)
        self.manifest = Manifest.from_dict(ctx.manifest)
        self.grid_paths = {k: Path(v) for k, v in ctx.grid_paths.items()}
        self.settings = {layer: self.manifest.layer_settings_hash(layer) for layer in self.manifest.layers}
        self._transforms: dict[tuple, DatumTransform] = {}
        self.index = {layer: self._index(layer) for layer in ("terrain", "imagery")}
        self.balance: Balance | None = None
        self.balance_sha = ""
        self.ref_interior = None
        self.water: Water | None = None
        self.water_shas: list[str] = []
        s = self.manifest.layers["imagery"].get("balance")
        path = self.pkg / "imagery" / BALANCE_FILE
        if s and path.exists():
            data = path.read_bytes()
            self.balance, self.balance_sha = Balance.from_json(data), sha256_bytes(data)
            fps = reference_footprints(self.manifest, s["reference"])
            if fps:
                self.ref_interior = ref_interior(fps, s["feather_m"])
            classes, shas = class_rasters(self.manifest, ctx.asset_paths)
            if classes and s.get("water_fade_m"):  # a balance written before the water keys: no mask, as before
                self.water = Water(tuple(classes), float(s.get("naip_water_buffer_m", 0.0)), float(s["water_fade_m"]))
                self.water_shas = sorted(shas)
        self.ndvi_fit: NdviFit | None = None
        self.ndvi_fit_sha = ""
        self.ndvi_interior = None
        self.ndvi_water: Water | None = None  # None with a zero buffer: the leaves then skip the land distance
        self.ndvi_water_shas: list[str] = []
        ns = self.manifest.layers.get("ndvi")
        if ns:
            self.index["ndvi"] = self._index("ndvi")
            path = self.pkg / "ndvi" / NDVI_FIT_FILE
            if path.exists():
                data = path.read_bytes()
                self.ndvi_fit, self.ndvi_fit_sha = NdviFit.from_json(data), sha256_bytes(data)
            if ns["reference"]:
                fps = reference_footprints(self.manifest, ns["reference"])
                if fps:
                    self.ndvi_interior = ref_interior(fps, ns["feather_m"])
            if ns["naip_water_buffer_m"] > 0:
                classes, shas = class_rasters(self.manifest, ctx.asset_paths)
                if classes:
                    self.ndvi_water = Water(tuple(classes), float(ns["naip_water_buffer_m"]), 0.0)
                    self.ndvi_water_shas = sorted(shas)

    def _transform(self, datum_id: str, vertical_asset: str | None) -> DatumTransform:
        key = (datum_id, vertical_asset)
        if key not in self._transforms:
            sampler = None
            if vertical_asset:
                geoid = SourceRaster(path=Path(self.ctx.asset_paths[vertical_asset]), datum="wgs84", clamp_edges=True)

                def sampler(lon, lat, target_m=None, g=geoid):
                    t = geoid_target_m(lon, lat) if target_m is None else max(GEOID_TARGET_M, target_m)
                    return np.nan_to_num(g.sample(lon, lat, t)[0][0])

            self._transforms[key] = DatumTransform(self.manifest.datum["datums"][datum_id], self.grid_paths, sampler)
        return self._transforms[key]

    def _index(self, layer: str) -> LayerIndex:
        entries, geoms = [], []
        for p, sid in enumerate(self.manifest.layers[layer]["priorities"]):
            rec = self.manifest.source(sid)
            src = make_source(rec.id, rec.adapter, rec.options)
            data = [a for a in rec.assets if a.role == "data"]
            group_rank: dict[str, int] = {}
            for a in data:
                group_rank[a.group] = min(group_rank.get(a.group, a.rank), a.rank)
            for a in data:
                geom = shapely.box(*GLOBE) if src.global_coverage else _footprint(a, use_bbox=False)
                if geom is None:
                    continue  # the asset holds no data
                qid = f"{sid}/{a.id}"
                opener = src.ndvi_raster if layer == "ndvi" else src.open
                raster = opener(Path(self.ctx.asset_paths[qid]), to_asset(a))
                if raster is None:
                    continue  # no near-infrared band
                order = (p, group_rank[a.group], a.group, a.rank, a.id)
                entries.append(
                    Entry(
                        qid, a.sha256, order, src.max_zoom, raster, self._transform(raster.datum, raster.vertical_asset)
                    )
                )
                geoms.append(geom)
        return LayerIndex(entries, geoms)
