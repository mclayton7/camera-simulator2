"""Meta/WRI CHMv2 canopy height (REALISM R4, CC BY 4.0): one GeoTIFF per level-10 web-mercator quadkey, uint8 metres,
EPSG:3857 at ~1.19 m, 512-px tiles, deflate (~141 MB each), public on S3 over HTTPS. Discovery HEADs each quadkey over the
package bbox (a missing key, e.g. open ocean, is skipped) and reads its acquisition dates from metadata/<qk>.geojson.
Fetch downloads whole files into the cache; the canopy layer (layers/canopy.py) reads them. Import-light (math only)."""

from __future__ import annotations

import math

from .base import Area, Asset, Layer, SourceBase

BASE = "https://dataforgood-fb-data.s3.amazonaws.com/forests/v2/global/dinov3_global_chm_v2_ml3"
CHM_URL = BASE + "/chm/{qk}.tif"
META_URL = BASE + "/metadata/{qk}.geojson"
LEVEL = 10
R = 6378137.0
ATTRIBUTION = (
    "Meta and World Resources Institute (WRI), 2026. Version 2 High Resolution Canopy Height Maps (CHMv2), "
    "https://registry.opendata.aws/dataforgood-fb-forestsv2. Source imagery for CHM © 2016 Vantor"
)


def tile_xy(lon: float, lat: float, z: int = LEVEL) -> tuple[int, int]:
    n = 2**z
    lat = max(min(lat, 85.05112878), -85.05112878)
    x = int((lon + 180.0) / 360.0 * n)
    s = math.sin(math.radians(lat))
    y = int((0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * n)
    return min(max(x, 0), n - 1), min(max(y, 0), n - 1)


def quadkey(x: int, y: int, z: int = LEVEL) -> str:
    return "".join(str(((x >> b) & 1) + 2 * ((y >> b) & 1)) for b in range(z - 1, -1, -1))


def quadkey_xy(qk: str) -> tuple[int, int]:
    x = y = 0
    for ch in qk:
        d = int(ch)
        x, y = 2 * x + (d & 1), 2 * y + (d >> 1)
    return x, y


def quadkeys_for_bbox(w: float, s: float, e: float, n: float, z: int = LEVEL) -> list[str]:
    x0, y0 = tile_xy(w, n, z)
    x1, y1 = tile_xy(e, s, z)
    return sorted(quadkey(x, y, z) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1))


def quadkey_bounds_3857(qk: str) -> tuple[float, float, float, float]:
    x, y = quadkey_xy(qk)
    size = 2 * math.pi * R / 2 ** len(qk)
    minx = -math.pi * R + x * size
    maxy = math.pi * R - y * size
    return minx, maxy - size, minx + size, maxy


class Chmv2(SourceBase):
    id = "chmv2"
    layer = Layer.CANOPY
    licence = "CC-BY-4.0"
    dataset = "Meta/WRI High Resolution Canopy Height Maps v2 (CHMv2)"
    version = "dinov3_global_chm_v2_ml3"
    attribution = ATTRIBUTION
    area_kind = "bbox"
    datum = None

    def discover(self, area: Area) -> list[Asset]:
        from ..net import HttpError  # requests-backed: imported lazily so the module stays import-light

        assets = []
        for qk in quadkeys_for_bbox(*area.bounds):
            url = CHM_URL.format(qk=qk)
            status, size = self.http.head(url)
            if status in (403, 404):  # S3 answers 403 for a missing key without list permission
                continue
            if status != 200:
                raise HttpError(f"HEAD {url}: HTTP {status}")
            meta: dict = {"quadkey": qk}
            try:
                dates = sorted({f["properties"]["acq_date"] for f in self.http.get_json(META_URL.format(qk=qk))["features"]
                                if f.get("properties", {}).get("acq_date")})
                if dates:
                    meta["acquired"] = [dates[0], dates[-1]]
            except (HttpError, KeyError, TypeError):
                pass  # dates are informational
            assets.append(Asset(id=qk, url=url, size=size, group=self.id, rank=len(assets), metadata=meta))
        return assets

    def prepare(self, path, asset, cache):
        return None
