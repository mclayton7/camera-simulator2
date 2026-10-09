"""USDA NAIP from Microsoft Planetary Computer (STAC `naip`; CA newest there is 2022, 0.6 m RGBN COGs in
NAD83 UTM, nodata 0 in all bands). Manifest URLs are unsigned; fetch signs them with a SAS token and re-signs
when a token is refused or about to expire."""

from __future__ import annotations

import datetime as dt
import logging
import time
import urllib.parse
from pathlib import Path

from ..net import Http
from .base import Area, Asset, Layer, SourceBase, SourceRaster

log = logging.getLogger(__name__)
STAC_SEARCH = "https://planetarycomputer.microsoft.com/api/stac/v1/search"
SAS_TOKEN = "https://planetarycomputer.microsoft.com/api/sas/v1/token/{account}/{container}"
REFRESH_BEFORE_S = 300


class PcSigner:
    """Appends a per-container SAS token to blob URLs. The token is never logged or put in an exception."""

    def __init__(self, http, now=time.time):
        self.http, self.now = http, now
        self._tokens: dict[tuple[str, str], tuple[str, float]] = {}

    def __call__(self, url: str, force: bool = False) -> str:
        p = urllib.parse.urlparse(url)
        key = (p.netloc.split(".")[0], p.path.lstrip("/").split("/")[0])
        tok = self._tokens.get(key)
        if force or tok is None or tok[1] - REFRESH_BEFORE_S <= self.now():
            d = self.http.get_json(SAS_TOKEN.format(account=key[0], container=key[1]))
            expiry = dt.datetime.fromisoformat(d["msft:expiry"]).timestamp()
            tok = (d["token"], expiry)
            self._tokens[key] = tok
        return f"{url}?{tok[0]}"


class NaipPc(SourceBase):
    id = "naip_pc"
    layer = Layer.IMAGERY
    licence = "LicenseRef-PublicDomain-USGov"
    dataset = "USDA NAIP (Microsoft Planetary Computer)"
    attribution = "USDA Farm Production and Conservation - Business Center, Geospatial Enterprise Operations (NAIP)"
    max_zoom = 17
    area_kind = "margin"
    datum = "nad83_2011"

    def __init__(self, options=None, http=None):
        super().__init__(options, http)
        self._signer: PcSigner | None = None

    def discover(self, area: Area) -> list[Asset]:
        year = str(self._options.get("year", "2022"))
        body = {"collections": ["naip"], "bbox": list(area.bounds), "limit": 250}
        items: list[dict] = []
        d = self.http.post_json(STAC_SEARCH, body)
        while True:  # STAC API paging: follow `next` links (POST with a merged body, or GET) to the last page
            items += d.get("features", [])
            nxt = next((link for link in d.get("links", []) if link.get("rel") == "next"), None)
            if nxt is None:
                break
            if nxt.get("method", "GET").upper() == "POST":
                body = {**body, **nxt.get("body", {})}
                d = self.http.post_json(nxt["href"], body)
            else:
                d = self.http.get_json(nxt["href"])
        items = sorted((i for i in items if str(i["properties"].get("naip:year")) == year), key=lambda i: i["id"])
        if not items:
            log.warning("naip_pc: no NAIP %s items in %s", year, area.bounds)
        self.version = year
        return [
            Asset(
                id=i["id"],
                url=i["assets"]["image"]["href"],
                group=self.id,
                rank=r,
                metadata={
                    "bbox": i["bbox"],
                    "epsg": i["properties"].get("proj:epsg"),
                    "datetime": i["properties"].get("datetime"),
                },
            )
            for r, i in enumerate(items)
        ]

    def sign(self, url: str, force: bool = False) -> str:
        if self._signer is None:
            self._signer = PcSigner(self.http or Http())
        return self._signer(url, force)

    def open(self, path: Path, asset: Asset) -> SourceRaster:
        return SourceRaster(path=Path(path), datum=self.datum, bands=(1, 2, 3), nodata_rule="all_zero")
