"""Bridge decks (REALISM R3, spec docs/superpowers/specs/2026-10-15-bridges-ripples-design.md section 1): OSM bridge
ways -> deck heights from the bare-earth terrain at their grounded ends -> a swept deck cross-section with parapets and
piers -> 3D Tiles 1.1 under features/bridges/."""

from __future__ import annotations

BRIDGES_RULES_VERSION = "1"  # bump on any change to selection, heights or geometry (it is in the layer settings hash)
MIN_CLEARANCE_M = 2.0  # a deck never higher than this above the terrain is not built (the terrain carries the road)
DECK_THICKNESS_M = 1.5
PARAPET_W_M, PARAPET_H_M = 0.3, 1.0
PIER_SPACING_M = 30.0
PIER_MIN_CLEARANCE_M = 3.0
PIER_END_MARGIN_M = 10.0
PIER_ALONG_M = 1.2
PIER_ACROSS_FRACTION = 0.6
PIER_SINK_M = 0.5
STATION_MAX_M = 10.0
PROFILE_STEP_M = 5.0
TILE_ZOOM = 13
GEOMETRIC_ERROR = 60.0
