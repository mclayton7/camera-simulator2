"""Buildings layer (REALISM R3): Overture footprints -> extruded flat-roof meshes -> 3D Tiles 1.1."""

from __future__ import annotations

BUILDINGS_RULES_VERSION = "1"  # bump when geometry/height/tier rules change (it is in the layer settings hash)
DEFAULT_RELEASE = "2026-09-23.1"
TIER_MIN_AREA_M2 = {13: 2000.0, 14: 300.0}  # else LEAF_ZOOM
LEAF_ZOOM = 15
GEOMETRIC_ERROR = {"root": 200.0, 13: 60.0, 14: 20.0, 15: 6.0}
