"""Overture Maps buildings theme (ODbL 1.0; REALISM R3). Overture deletes releases after 60 days, so the bbox query's
result is the snapshot: a GeoParquet in the fetch cache, keyed by the release and bbox (the asset URL), whose sha256 the
manifest pins. Builds read the snapshot only. Discovery makes no request.

The query reads s3://overturemaps-us-west-2/release/<release>/theme=buildings/type=building/* anonymously through
DuckDB's httpfs extension (DuckDB installs it on first use) and filters on Overture's `bbox` struct column, so the
spatial extension is not needed; geometry stays WKB (BLOB). Option `parquet` replaces the S3 glob with a local file
(tests)."""

from __future__ import annotations

import uuid
from pathlib import Path

from ..cache import CacheError
from .base import Area, Asset, Layer, SourceBase

S3_GLOB = "s3://overturemaps-us-west-2/release/{release}/theme=buildings/type=building/*"
COLUMNS = "id, geometry, height, num_floors, class, subtype, to_json(sources) AS sources"


def snapshot_url(release: str, bounds) -> str:
    w, s, e, n = (float(v) for v in bounds)
    return f"overture://{release}/theme=buildings/type=building?bbox={w!r},{s!r},{e!r},{n!r}"


def _quote(text: str) -> str:
    return "'" + text.replace("'", "''") + "'"


def snapshot_sql(src: str, bounds, out: Path) -> str:
    w, s, e, n = (float(v) for v in bounds)
    return (
        f"COPY (SELECT {COLUMNS} FROM read_parquet({_quote(src)}, hive_partitioning = true) "
        f"WHERE bbox.xmin <= {e!r} AND bbox.xmax >= {w!r} AND bbox.ymin <= {n!r} AND bbox.ymax >= {s!r} "
        f"ORDER BY id) TO {_quote(str(out))} (FORMAT parquet, COMPRESSION zstd)"
    )


def connect():
    """A DuckDB connection that keeps GeoParquet geometry as WKB bytes."""
    import duckdb

    con = duckdb.connect()
    try:
        con.execute("SET enable_geoparquet_conversion = false")
    except duckdb.Error:  # a DuckDB without the setting never converts without the spatial extension
        pass
    return con


class OvertureBuildings(SourceBase):
    id = "overture_buildings"
    layer = Layer.BUILDINGS
    licence = "ODbL-1.0"
    dataset = "Overture Maps buildings"
    attribution = (
        "© OpenStreetMap contributors, Overture Maps Foundation (also: Esri Community Maps, Microsoft Global ML "
        "Building Footprints, Google Open Buildings, USGS 3DEP; docs.overturemaps.org/attribution)"
    )
    area_kind = "bbox"

    def __init__(self, options: dict | None = None, http=None):
        super().__init__(options, http)
        self.version = str(self._options.get("release", ""))
        if not self.version:
            raise ValueError("overture_buildings needs a release option")

    def discover(self, area: Area) -> list[Asset]:
        return [
            Asset(
                id=f"buildings-{self.version}",
                url=snapshot_url(self.version, area.bounds),
                group=self.id,
                metadata={"release": self.version, "bbox": [float(v) for v in area.bounds]},
            )
        ]

    def prepare(self, path, asset, cache):
        return None

    def fetch_blob(self, asset: Asset, cache) -> tuple[Path, str, int]:
        """The snapshot's (path, sha256, size): the cached blob when there is one, else run the query once."""
        if asset.sha256 and cache.blob_path(asset.sha256).exists():
            p = cache.blob_path(asset.sha256)
            return p, asset.sha256, p.stat().st_size
        hit = cache.lookup(asset.url, asset.sha256)  # indexed by URL from an earlier fetch; never downloads
        if hit is not None:
            return hit
        src = self._options.get("parquet") or S3_GLOB.format(release=self.version)
        tmp = cache.root / "tmp" / f"{uuid.uuid4().hex}.parquet"
        tmp.parent.mkdir(parents=True, exist_ok=True)
        con = connect()
        try:
            if "parquet" not in self._options:
                con.execute("INSTALL httpfs; LOAD httpfs; SET s3_region = 'us-west-2'")
            con.execute(snapshot_sql(src, asset.metadata["bbox"], tmp))
        except Exception as e:  # noqa: BLE001 - duckdb.IOException/HTTPException: release deleted or offline
            tmp.unlink(missing_ok=True)
            raise CacheError(
                f"Overture release {self.version} could not be read ({e}); no snapshot of it is cached. Overture keeps "
                "releases 60 days: pick a current release in [buildings] release, or restore the cache"
            ) from e
        finally:
            con.close()
        path, sha, size = cache.put_file(asset.url, tmp)
        if asset.sha256 and sha != asset.sha256:
            raise CacheError(f"Overture {self.version} snapshot sha256 {sha} but the manifest pins {asset.sha256}")
        return path, sha, size
