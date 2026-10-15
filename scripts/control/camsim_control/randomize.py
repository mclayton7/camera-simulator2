"""Seeded scene sampler for datasets (docs/control-api.md). Sample i depends only on (seed, i)."""

from __future__ import annotations

import math
import random
from dataclasses import dataclass, field

EARTH_R = 6378137.0


@dataclass(frozen=True)
class EntitySpec:
    type_id: int
    surface: str  # none | ground | water


@dataclass(frozen=True)
class Sample:
    index: int
    aim: tuple[float, float]
    camera: dict
    utc: str
    environment: dict
    entities: list[dict] = field(default_factory=list)


def offset_ll(lat: float, lon: float, north_m: float, east_m: float) -> tuple[float, float]:
    """A local tangent-plane offset (spherical; metres-level over a few km, enough for sampling)."""
    dlat = math.degrees(north_m / EARTH_R)
    dlon = math.degrees(east_m / (EARTH_R * math.cos(math.radians(lat))))
    return lat + dlat, lon + dlon


def sample(seed: int, index: int, site: tuple[float, float], ground_alt: float, types: list[EntitySpec],
           date: str = "2026-06-15") -> Sample:
    rng = random.Random(f"{seed}:{index}")
    # Aim point within 50 m of the site
    aim = offset_ll(site[0], site[1], rng.uniform(-50, 50), rng.uniform(-50, 50))
    # Camera: slant range, depression, heading, FOV; positioned so the boresight hits the aim point
    slant = rng.uniform(300.0, 2000.0)
    depression = rng.uniform(20.0, 90.0)
    heading = rng.uniform(0.0, 360.0)
    hfov = rng.uniform(5.0, 40.0)
    horiz = slant * math.cos(math.radians(depression))
    height = slant * math.sin(math.radians(depression))
    h = math.radians(heading)
    cam_lat, cam_lon = offset_ll(aim[0], aim[1], -horiz * math.cos(h), -horiz * math.sin(h))
    camera = {"lat": cam_lat, "lon": cam_lon, "alt": ground_alt + height, "heading": heading, "pitch": 0.0,
              "roll": 0.0, "gimbal": {"yaw": 0.0, "pitch": -depression, "roll": 0.0}, "hfov_deg": hfov}
    # Entities within 0.4 x the footprint half-width at the aim point
    half_width = slant * math.tan(math.radians(hfov / 2))
    entities = []
    for _ in range(rng.randint(1, 4)):
        spec = rng.choice(types)
        r = 0.4 * half_width * math.sqrt(rng.random())
        a = rng.uniform(0, 2 * math.pi)
        lat, lon = offset_ll(aim[0], aim[1], r * math.cos(a), r * math.sin(a))
        entities.append({"type_id": spec.type_id, "lat": lat, "lon": lon, "alt": ground_alt,
                         "heading": rng.uniform(0.0, 360.0), "surface": spec.surface})
    # Time: local solar 07-17 h at the site, as UTC
    local_h = rng.uniform(7.0, 17.0)
    utc_h = (local_h - site[1] / 15.0) % 24.0
    hh, mm = int(utc_h), int((utc_h % 1) * 60)
    utc = f"{date}T{hh:02d}:{mm:02d}:00Z"
    environment = {
        "atmosphere": {"visibility_m": round(rng.uniform(5000, 50000))},
        "clouds": {"coverage_pct": 0.0 if rng.random() < 0.5 else round(rng.uniform(0, 60), 1)},
        "sea": {"beaufort": round(rng.uniform(0, 4), 1)},
    }
    return Sample(index=index, aim=aim, camera=camera, utc=utc, environment=environment, entities=entities)
