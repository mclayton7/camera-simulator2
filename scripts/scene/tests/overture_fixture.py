"""A small Overture-shaped buildings GeoParquet (id, geometry WKB, bbox struct, height, num_floors, class, subtype,
sources), written with DuckDB so the real snapshot SQL runs against it offline."""

from __future__ import annotations

from pathlib import Path

import duckdb
import shapely


def write_overture_parquet(path: Path, rows: list[dict]) -> Path:
    """rows: {id, geom (shapely, lon/lat), height?, num_floors?, class?, subtype?}."""
    con = duckdb.connect()
    con.execute(
        "CREATE TABLE b (id VARCHAR, geometry BLOB, "
        "bbox STRUCT(xmin DOUBLE, xmax DOUBLE, ymin DOUBLE, ymax DOUBLE), "
        "height DOUBLE, num_floors INTEGER, class VARCHAR, subtype VARCHAR, sources STRUCT(dataset VARCHAR)[])"
    )
    for r in rows:
        g = r["geom"]
        w, s, e, n = g.bounds
        con.execute(
            "INSERT INTO b VALUES (?, ?, {'xmin': ?, 'xmax': ?, 'ymin': ?, 'ymax': ?}, ?, ?, ?, ?, [{'dataset': 'OpenStreetMap'}])",
            [r["id"], shapely.to_wkb(g), w, e, s, n, r.get("height"), r.get("num_floors"), r.get("class"), r.get("subtype")],
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    con.execute(f"COPY b TO '{str(path).replace(chr(39), chr(39) * 2)}' (FORMAT parquet)")
    return path


def box_lonlat(lon: float, lat: float, w_m: float, h_m: float):
    """An axis-aligned w_m x h_m rectangle centred on (lon, lat)."""
    import math

    dlat = h_m / 2 / 111320.0
    dlon = w_m / 2 / (111320.0 * math.cos(math.radians(lat)))
    return shapely.box(lon - dlon, lat - dlat, lon + dlon, lat + dlat)
