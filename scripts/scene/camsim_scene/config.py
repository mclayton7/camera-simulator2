"""scene.toml -> ScenePlan: validation, profile defaults, regions (globe / ring / margin / bbox).

name = "pendleton"
bbox = [-117.62, 33.19, -117.24, 33.52]   # W S E N, degrees; must not cross the antimeridian
profile = "sim"                            # preview | sim
seed = 0
ring_km = 100
bmng_month = 7
jpeg_quality = 85
imagery_margin_km = 3                      # NAIP fetched and built this far beyond the bbox (<= ring_km)
balance = true                             # colour-match Sentinel-2 to NAIP and feather NAIP's edge (only where they share land)
naip_water_buffer_m = 200                  # NAIP only this far beyond WorldCover water's edge (0 = off)
ndvi = true                               # NDVI layer beside the imagery (sim default; preview false)
ndvi_max_zoom = 15                        # NDVI pyramid depth, [10, 17]
allow = []                                 # extra licence ids (see licences.toml)
[priorities]                               # optional; defaults from the profile
terrain = ["dep3_1m", "dep3_13", "etopo2022"]
[sources.naip_pc]                          # per-source options; `adapter` picks a non-default class
year = "2022"
[zoom.bbox]                                # optional zoom overrides per region
imagery = 17
"""

from __future__ import annotations

import math
import re
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from .tiling import GLOBE, Bounds, Region

LAYERS = ("terrain", "imagery", "landcover")
TILED_LAYERS = ("terrain", "imagery")
KM_PER_DEG = 111.32
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
KEYS = {
    "name",
    "bbox",
    "profile",
    "seed",
    "ring_km",
    "bmng_month",
    "jpeg_quality",
    "imagery_margin_km",
    "balance",
    "naip_water_buffer_m",
    "allow",
    "priorities",
    "sources",
    "zoom",
    "ndvi",
    "ndvi_max_zoom",
}

PROFILES = {
    "preview": {
        "priorities": {"terrain": ["dep3_13", "etopo2022"], "imagery": ["wc_s2", "bmng"], "landcover": ["worldcover"]},
        "bbox_zoom": {"terrain": 14, "imagery": 13},
        "ndvi": False,
    },
    "sim": {
        "priorities": {
            "terrain": ["dep3_1m", "dep3_13", "etopo2022"],
            "imagery": ["naip_pc", "wc_s2", "bmng"],
            "landcover": ["worldcover"],
        },
        "bbox_zoom": {"terrain": 16, "imagery": 17},
        "ndvi": True,
    },
}
GLOBE_ZOOM = {"terrain": 8, "imagery": 8}
RING_ZOOM = {"terrain": 10, "imagery": 10}
DEFAULT_SOURCE_OPTIONS = {"naip_pc": {"year": "2022"}}
TERRAIN_GRID = 257
FEATHER_M = 30.0
BALANCE_REFERENCE, BALANCE_TARGET = "naip_pc", "wc_s2"
BALANCE = {  # Sentinel-2 -> NAIP colour match and NAIP edge feather (balance.py, layers/imagery.py)
    "fit_step_m": 10.0,
    "cell_km": 2.0,
    "min_cell_samples": 500,
    "median_filter": 3,
    "decay_km": 10.0,
    "quantiles": 257,
    "exclude_classes": [0, 80],  # WorldCover no data, permanent water
    "feather_m": 200.0,
    "water_fade_m": 100.0,  # the colour match fades out to raw Sentinel-2 over this much WorldCover water (water.py)
}
NAIP_WATER_BUFFER_M = 200.0  # default naip_water_buffer_m: NAIP kept within this distance of land
MAX_WATER_BUFFER_M = 5000.0
NDVI_REFERENCE, NDVI_TARGET = "naip_pc", "wc_s2"
NDVI = {  # NDVI layer (layers/ndvi.py) and its NAIP -> Sentinel-2 fit (ndvi_fit.py)
    "feather_m": BALANCE["feather_m"],
    "fit_step_m": 10.0,
    "min_samples": 500,
    "exclude_classes": [0, 80],  # WorldCover no data, permanent water
}
NDVI_MAX_ZOOM = 15
TILE_PX = 256
TILING = {  # OGC TMS 2.0 WorldCRS84Quad, addressed TMS-style (y from the south) as Cesium expects
    "tile_matrix_set": "WorldCRS84Quad",
    "tile_matrix_set_uri": "http://www.opengis.net/def/tilematrixset/OGC/1.0/WorldCRS84Quad",
    "scheme": "tms",
    "projection": "EPSG:4326",
    "root_tiles": [2, 1],
    "tile_px": 256,
    "y_origin": "south",
}


class ConfigError(ValueError):
    pass


def validate_bbox(b) -> Bounds:
    if not isinstance(b, (list, tuple)) or len(b) != 4:
        raise ConfigError(f"bbox must be 4 numbers W S E N, got {b!r}")
    w, s, e, n = (float(v) for v in b)
    if not all(math.isfinite(v) for v in (w, s, e, n)):
        raise ConfigError(f"bbox must be finite, got {b!r}")
    if not (-180.0 <= w <= 180.0 and -180.0 <= e <= 180.0 and -90.0 <= s <= 90.0 and -90.0 <= n <= 90.0):
        raise ConfigError(f"bbox outside [-180, 180] x [-90, 90]: {b!r}")
    if w >= e:
        raise ConfigError(f"bbox needs W < E (a bbox must not cross the antimeridian; split it): {b!r}")
    if s >= n:
        raise ConfigError(f"bbox needs S < N: {b!r}")
    return (w, s, e, n)


def ring_bounds(bbox: Bounds, ring_km: float) -> Bounds:
    """bbox grown by ring_km on every side; the longitude pad uses the pole-ward latitude (never too small)."""
    w, s, e, n = bbox
    dlat = ring_km / KM_PER_DEG
    lat = min(89.0, max(abs(s), abs(n)) + dlat)
    dlon = ring_km / (KM_PER_DEG * math.cos(math.radians(lat)))
    return (max(-180.0, w - dlon), max(-90.0, s - dlat), min(180.0, e + dlon), min(90.0, n + dlat))


@dataclass(frozen=True)
class ScenePlan:
    name: str
    bbox: Bounds
    profile: str = "sim"
    seed: int = 0
    ring_km: float = 100.0
    bmng_month: int = 7
    jpeg_quality: int = 85
    priorities: dict = field(default_factory=dict)
    source_options: dict = field(default_factory=dict)
    zoom: dict = field(default_factory=dict)
    allow: tuple[str, ...] = ()
    imagery_margin_km: float = 3.0
    balance: bool = True
    naip_water_buffer_m: float = NAIP_WATER_BUFFER_M
    ndvi: bool = False
    ndvi_max_zoom: int = NDVI_MAX_ZOOM

    def ring_bounds(self) -> Bounds:
        return ring_bounds(self.bbox, self.ring_km)

    def area(self, kind: str) -> Bounds:
        return {
            "globe": GLOBE,
            "ring": self.ring_bounds(),
            "margin": ring_bounds(self.bbox, self.imagery_margin_km),
            "bbox": self.bbox,
        }[kind]

    def regions(self) -> list[Region]:
        margin_zoom = {"terrain": self.zoom["ring"]["terrain"], "imagery": self.zoom["bbox"]["imagery"]}
        return [
            Region.from_bounds("globe", GLOBE, self.zoom["globe"]),
            Region.from_bounds("ring", self.ring_bounds(), self.zoom["ring"]),
            Region.from_bounds("margin", self.area("margin"), margin_zoom),
            Region.from_bounds("bbox", self.bbox, self.zoom["bbox"]),
        ]

    def source_ids(self) -> list[str]:
        out: list[str] = []
        for layer in LAYERS:
            out += [s for s in self.priorities[layer] if s not in out]
        return out


def _int(data: dict, key: str, default: int, lo: int, hi: int) -> int:
    v = data.get(key, default)
    if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
        raise ConfigError(f"{key} must be an integer in [{lo}, {hi}], got {v!r}")
    return v


def parse_scene(data: dict) -> ScenePlan:
    unknown = set(data) - KEYS
    if unknown:
        raise ConfigError(f"unknown key(s) in scene: {sorted(unknown)}")
    name = data.get("name")
    if not isinstance(name, str) or not NAME_RE.match(name):
        raise ConfigError(f"name must match {NAME_RE.pattern}, got {name!r}")
    bbox = validate_bbox(data.get("bbox"))
    profile = data.get("profile", "sim")
    if profile not in PROFILES:
        raise ConfigError(f"profile must be one of {sorted(PROFILES)}, got {profile!r}")
    ring_km = float(data.get("ring_km", 100.0))
    if not 0.0 <= ring_km <= 1000.0:
        raise ConfigError(f"ring_km must be in [0, 1000], got {ring_km}")
    month = _int(data, "bmng_month", 7, 1, 12)
    margin_km = float(data.get("imagery_margin_km", 3.0))
    if not 0.0 <= margin_km <= 50.0:
        raise ConfigError(f"imagery_margin_km must be in [0, 50], got {margin_km}")
    if margin_km > ring_km:
        raise ConfigError(f"imagery_margin_km ({margin_km}) must not exceed ring_km ({ring_km})")
    balance = data.get("balance", True)
    if not isinstance(balance, bool):
        raise ConfigError(f"balance must be true or false, got {balance!r}")
    buffer_m = data.get("naip_water_buffer_m", NAIP_WATER_BUFFER_M)
    lo = BALANCE["feather_m"]  # a narrower buffer would let the feather fade NAIP on land at the shore
    if (
        isinstance(buffer_m, bool)
        or not isinstance(buffer_m, (int, float))
        or not (buffer_m == 0 or lo <= buffer_m <= MAX_WATER_BUFFER_M)
    ):
        raise ConfigError(
            f"naip_water_buffer_m must be 0 (off) or in [{lo:g}, {MAX_WATER_BUFFER_M:g}], got {buffer_m!r}"
        )
    ndvi_given = "ndvi" in data
    ndvi = data.get("ndvi", PROFILES[profile]["ndvi"])
    if not isinstance(ndvi, bool):
        raise ConfigError(f"ndvi must be true or false, got {ndvi!r}")
    ndvi_max_zoom = _int(data, "ndvi_max_zoom", NDVI_MAX_ZOOM, 10, 17)
    quality = _int(data, "jpeg_quality", 85, 1, 95)
    seed = _int(data, "seed", 0, 0, 2**31 - 1)
    prio = {k: list(v) for k, v in PROFILES[profile]["priorities"].items()}
    for layer, ids in data.get("priorities", {}).items():
        if layer not in LAYERS:
            raise ConfigError(f"priorities.{layer}: unknown layer (one of {LAYERS})")
        if not isinstance(ids, list) or not all(isinstance(i, str) for i in ids):
            raise ConfigError(f"priorities.{layer} must be a list of source ids")
        prio[layer] = list(ids)
    if ndvi and ndvi_given and not any(s in prio["imagery"] for s in (NDVI_REFERENCE, NDVI_TARGET)):
        raise ConfigError("ndvi = true needs naip_pc or wc_s2 in priorities.imagery")
    options = {k: dict(v) for k, v in DEFAULT_SOURCE_OPTIONS.items()}
    for sid, opts in data.get("sources", {}).items():
        if not isinstance(opts, dict):
            raise ConfigError(f"sources.{sid} must be a table")
        options.setdefault(sid, {}).update(opts)
    options.setdefault("bmng", {})["month"] = month
    zoom = {
        "globe": dict(GLOBE_ZOOM),
        "ring": dict(RING_ZOOM),
        "bbox": dict(PROFILES[profile]["bbox_zoom"]),
    }
    for region, z in data.get("zoom", {}).items():
        if region not in zoom or not isinstance(z, dict):
            raise ConfigError(f"zoom.{region}: unknown region (one of {sorted(zoom)})")
        for layer, v in z.items():
            if layer not in TILED_LAYERS or not isinstance(v, int) or not 0 <= v <= 22:
                raise ConfigError(f"zoom.{region}.{layer} must be an integer in [0, 22]")
            zoom[region][layer] = v
    allow = data.get("allow", [])
    if not isinstance(allow, list) or not all(isinstance(a, str) for a in allow):
        raise ConfigError("allow must be a list of licence ids")
    return ScenePlan(
        name,
        bbox,
        profile,
        seed,
        ring_km,
        month,
        quality,
        prio,
        options,
        zoom,
        tuple(allow),
        imagery_margin_km=margin_km,
        balance=balance,
        naip_water_buffer_m=float(buffer_m),
        ndvi=ndvi,
        ndvi_max_zoom=ndvi_max_zoom,
    )


def load_scene(path: Path) -> ScenePlan:
    try:
        data = tomllib.loads(Path(path).read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as e:
        raise ConfigError(f"{path}: {e}") from e
    return parse_scene(data)


def balance_settings(plan: ScenePlan) -> dict | None:
    """The colour-match settings, or None when it is off or the package lacks NAIP or Sentinel-2."""
    prio = plan.priorities["imagery"]
    if not plan.balance or BALANCE_REFERENCE not in prio or BALANCE_TARGET not in prio:
        return None
    return {
        "reference": BALANCE_REFERENCE,
        "target": BALANCE_TARGET,
        **BALANCE,
        "naip_water_buffer_m": plan.naip_water_buffer_m,
    }


def ndvi_settings(plan: ScenePlan) -> dict | None:
    """The NDVI layer's settings, or None when it is off or the imagery has neither NAIP nor Sentinel-2."""
    prio = [s for s in plan.priorities["imagery"] if s in (NDVI_REFERENCE, NDVI_TARGET)]
    if not plan.ndvi or not prio:
        return None
    return {
        "priorities": prio,
        "reference": NDVI_REFERENCE if NDVI_REFERENCE in prio else None,
        "target": NDVI_TARGET if NDVI_TARGET in prio else None,
        "max_zoom": plan.ndvi_max_zoom,
        "tile_px": TILE_PX,
        "format": "png",
        "encoding": "L8: 0 nodata, 1 + floor((ndvi + 1) * 127 + 0.5)",
        **NDVI,
        "naip_water_buffer_m": plan.naip_water_buffer_m,
    }


def layer_settings(plan: ScenePlan) -> dict:
    """The manifest's `layers` section: everything per layer that tiles depend on besides regions and data."""
    out = {
        "terrain": {
            "priorities": plan.priorities["terrain"],
            "grid": TERRAIN_GRID,
            "feather_m": FEATHER_M,
            "max_error": "max(0.1, 0.25 * 77067 / 2^z) m",
            "format": "quantized-mesh-1.0",
            "extensions": ["octvertexnormals"],
            "gzip_level": 9,
        },
        "imagery": {
            "priorities": plan.priorities["imagery"],
            "tile_px": TILE_PX,
            "format": "jpeg",
            "jpeg_quality": plan.jpeg_quality,
            "subsampling": "4:2:0",
            "margin_km": plan.imagery_margin_km,
            "balance": balance_settings(plan),
        },
        "landcover": {"priorities": plan.priorities["landcover"], "bounds": list(plan.ring_bounds()), "tile_deg": 0.05},
    }
    ndvi = ndvi_settings(plan)
    if ndvi is not None:
        out["ndvi"] = ndvi
    return out


def default_scene_toml(name: str, bbox: Bounds, profile: str = "sim") -> str:
    w, s, e, n = validate_bbox(bbox)
    return (
        f"# camsim-scene scene file (docs/scene-packages.md)\n"
        f'name = "{name}"\n'
        f"bbox = [{w!r}, {s!r}, {e!r}, {n!r}]  # W S E N\n"
        f'profile = "{profile}"\n'
        f"seed = 0\n"
        f"ring_km = 100\n"
        f"bmng_month = 7\n"
        f"jpeg_quality = 85\n"
        f"imagery_margin_km = 3\n"
        f"balance = true\n"
        f"naip_water_buffer_m = 200\n"
    )
