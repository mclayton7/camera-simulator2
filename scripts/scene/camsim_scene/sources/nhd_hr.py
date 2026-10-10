"""USGS NHD High Resolution water (public domain; REALISM R2). NHD is retired (static since 2023), so the HU4
GeoPackages on TNM are reproducible as-is; 3DHP replaces this adapter when per-unit downloads exist. GeoPackages are read
with sqlite3 + shapely (no GDAL). Kept: perennial lakes/ponds (FType 390), reservoirs (436) and stream/river areas
(460); intermittent / ephemeral features, the sea, estuaries and every flowline are not.

Checked 2026-10-10 against a real download (TNM products API, dataset DATASET, prodFormats=GeoPackage): items carry
title ("... for Hydrological Unit (HU) 4 - 1807 (published ...)": the string "HU4" is NOT in the title, only in
downloadURL ".../NHD/HU4/GPKG/NHD_H_1807_HU4_GPKG.zip"), format "GeoPackage", sizeInBytes, downloadURL. The same query
also returns the National, State and HU8 zips, so HU4 items are picked by "_HU4_" in the URL. The 1807 zip is 166 MB
(456 MB unzipped). GPKG layout: tables NHDWaterbody and NHDArea (MULTIPOLYGON, srs_id 4269 = NAD83, taken as WGS-84:
about a metre, under a land-cover texel); geometry column "SHAPE"; lower-case attributes permanent_identifier, ftype,
fcode (NHDWaterbody also has fcode_description); lookup table NHDFCode(fcode, description), e.g. 39004 "Lake/Pond:
Hydrographic Category = Perennial", 43600 "Reservoir". Blob header flags are 0x05 (little endian, XYZ envelope).
Columns are matched case-insensitively."""

from __future__ import annotations

import sqlite3
import zipfile
from pathlib import Path

import shapely
from shapely.geometry import box

from ..features import Feature
from .base import Area, Asset, Layer, SourceBase
from .tnm import tnm_products

DATASET = "National Hydrography Dataset (NHD) Best Resolution"
FTYPES = {"NHDWaterbody": (390, 436), "NHDArea": (460,)}
_ENVELOPE = {0: 0, 1: 32, 2: 48, 3: 48, 4: 64}


def keep_fcode(ftype: int, description: str) -> bool:
    if ftype not in (390, 436, 460):
        return False
    d = description.lower()
    return "intermittent" not in d and "ephemeral" not in d


def gpkg_geometry(blob: bytes):
    if blob[:2] != b"GP":
        raise ValueError("not a GeoPackage geometry")
    flags = blob[3]
    env = _ENVELOPE[(flags >> 1) & 0x07]
    return shapely.from_wkb(blob[8 + env :])


def _cols(c, table) -> dict[str, str]:
    return {r[1].lower(): r[1] for r in c.execute(f'pragma table_info("{table}")')}


def read_nhd(gpkg: Path, bounds) -> list[Feature]:
    want = box(*bounds)
    c = sqlite3.connect(f"file:{gpkg}?mode=ro", uri=True)
    try:
        fc = _cols(c, "NHDFCode")
        desc = dict(c.execute(f'select "{fc["fcode"]}", "{fc["description"]}" from NHDFCode'))
        geom_col = dict(c.execute("select table_name, column_name from gpkg_geometry_columns"))
        out = []
        for table, ftypes in FTYPES.items():
            if table not in geom_col:
                continue
            cols = _cols(c, table)
            q = (
                f'select "{cols["permanent_identifier"]}", "{cols["ftype"]}", "{cols["fcode"]}", "{geom_col[table]}" '
                f'from "{table}"'
            )
            for pid, ftype, fcode, blob in c.execute(q):
                if ftype not in ftypes or blob is None or not keep_fcode(ftype, desc.get(fcode, "")):
                    continue
                g = gpkg_geometry(blob)
                if g.intersects(want):
                    out.append(Feature("water", f"nhd:{pid}", g, {"ftype": str(ftype), "fcode": str(fcode)}))
        out.sort(key=lambda f: f.source_id)
        return out
    finally:
        c.close()


class NhdHr(SourceBase):
    id = "nhd_hr"
    layer = Layer.FEATURES
    licence = "LicenseRef-PublicDomain-USGov"
    dataset = "USGS National Hydrography Dataset High Resolution"
    version = "HU4 GeoPackage (final, NHD retired 2023)"
    attribution = "USGS National Hydrography Dataset (NHD) High Resolution"
    area_kind = "margin"

    def discover(self, area: Area) -> list[Asset]:
        items = [
            i
            for i in tnm_products(self.http, area.bounds, DATASET)
            if "_HU4_" in i.get("downloadURL", "") and i.get("format", "GeoPackage").lower().startswith("geopackage")
        ]
        items.sort(key=lambda i: i["downloadURL"])
        return [
            Asset(
                id=Path(i["downloadURL"]).stem, url=i["downloadURL"], size=i.get("sizeInBytes"), group=self.id, rank=n
            )
            for n, i in enumerate(items)
        ]

    def prepare(self, path: Path, asset: Asset, cache) -> Path:
        out = cache.derived_path(asset.sha256 or Path(path).stem, ".gpkg")
        if not out.exists():
            out.parent.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(path) as z:
                name = next(n for n in z.namelist() if n.lower().endswith(".gpkg"))
                tmp = out.with_suffix(".tmp")
                with z.open(name) as src, open(tmp, "wb") as dst:
                    while chunk := src.read(1 << 20):
                        dst.write(chunk)
                tmp.rename(out)
        return out
