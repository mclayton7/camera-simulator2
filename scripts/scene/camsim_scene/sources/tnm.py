"""The National Map Access API (no auth; pages of up to 1000 products)."""

from __future__ import annotations

import time

from ..net import HttpError

TNM_PRODUCTS = "https://tnmaccess.nationalmap.gov/api/v1/products"


def _page(http, params: dict, attempts: int, sleep) -> dict:
    """One page. TNM sometimes answers HTTP 200 with an error body ({"error": ..., "toastMessage": ...}, no items):
    retried with backoff, then an HttpError, never read as "no products"."""
    for attempt in range(attempts):
        d = http.get_json(TNM_PRODUCTS, params)
        if "items" in d and "total" in d:
            return d
        if attempt + 1 < attempts:
            sleep(2.0**attempt)
    raise HttpError(f"GET {TNM_PRODUCTS}: error body after {attempts} attempts: {d.get('error', d)}")


def tnm_products(http, bounds, dataset: str, page: int = 1000, attempts: int = 5, sleep=time.sleep) -> list[dict]:
    items: list[dict] = []
    offset = 0
    while True:
        params = {
            "bbox": ",".join(f"{v:.6f}" for v in bounds),
            "datasets": dataset,
            "max": page,
            "offset": offset,
            "outputFormat": "JSON",
        }
        d = _page(http, params, attempts, sleep)
        batch = d.get("items", [])
        items += batch
        offset += len(batch)
        if not batch or offset >= int(d.get("total", 0)):
            return items
