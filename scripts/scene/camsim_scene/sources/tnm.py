"""The National Map Access API (no auth; pages of up to 1000 products)."""

from __future__ import annotations

TNM_PRODUCTS = "https://tnmaccess.nationalmap.gov/api/v1/products"


def tnm_products(http, bounds, dataset: str, page: int = 1000) -> list[dict]:
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
        d = http.get_json(TNM_PRODUCTS, params)
        batch = d.get("items", [])
        items += batch
        offset += len(batch)
        if not batch or offset >= int(d.get("total", 0)):
            return items
