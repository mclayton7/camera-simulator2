"""Per-process view of a fetched manifest: a SourceRaster and DatumTransform per data asset, ordered by layer
priority, behind an STRtree of footprints. BuildContext (plain data) is what crosses the process boundary;
each worker builds its WorkerState once."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
import shapely

from .datum import DatumTransform
from .manifest import AssetRecord, Manifest
from .sources import make_source
from .sources.base import M_PER_DEG, Asset, SourceRaster
from .tiling import GLOBE, Bounds, Coverage

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
                raster = src.open(Path(self.ctx.asset_paths[qid]), to_asset(a))
                order = (p, group_rank[a.group], a.group, a.rank, a.id)
                entries.append(
                    Entry(
                        qid, a.sha256, order, src.max_zoom, raster, self._transform(raster.datum, raster.vertical_asset)
                    )
                )
                geoms.append(geom)
        return LayerIndex(entries, geoms)
