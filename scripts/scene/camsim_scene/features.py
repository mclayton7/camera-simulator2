"""Vector features (roads, rail, paved areas, water) burned into land cover (REALISM R2)."""

from dataclasses import dataclass

RULES_VERSION = "1"


@dataclass(frozen=True)
class Feature:
    kind: str  # "road" | "paved_area" | "rail" | "water"
    source_id: str  # "osm:w123", "osm:a456", "nhd:<permanent id>"
    geom: object  # shapely geometry in lon/lat (LineString / Polygon / MultiPolygon)
    tags: dict  # the tags the rules read (str -> str)
