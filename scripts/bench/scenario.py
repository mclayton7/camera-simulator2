"""Deterministic CIGI flight for the render benchmark (ROADMAP 3A).

Phases fly around San Francisco (varied terrain, urban, water); far_origin jumps
~300 km east to exercise Cesium origin shift and lighting. Altitudes are WGS-84
ellipsoid heights, as CIGI 3.3 defines them. Times are UTC (the sim clock is UTC).
"""
from __future__ import annotations

import math
from dataclasses import dataclass, replace
from typing import Callable

import send_cigi_test as sc

BASE_LAT = 37.7749
BASE_LON = -122.4194
FAR_LON = BASE_LON + 3.41  # ~300 km east at 37.8 N (1 deg lon ~ 87.9 km)
CAMERA_ENTITY_ID = 1       # deploy/camsim_config.yaml camera_entity_id
GIMBAL_ART_PART_ID = 0     # camera gimbal (scripts/cigi_web_ui.py)


@dataclass(frozen=True)
class Pose:
    lat: float
    lon: float
    alt: float
    yaw: float = 0.0
    pitch: float = 0.0
    roll: float = 0.0
    gimbal_yaw: float = 0.0
    gimbal_pitch: float = -30.0
    fov_h: float = 60.0  # = sensor_fov_presets[0]; Sensor Control gain 0 re-applies it every frame
    sensor_id: int = 0   # 0 EO, 1 IR
    utc_hour: int = 19   # 12:00 PDT
    utc_minute: int = 0
    month: int = 6
    day: int = 21


@dataclass(frozen=True)
class Phase:
    name: str
    duration_s: float
    pose_at: Callable[[float], Pose]
    measured: bool = True


@dataclass(frozen=True)
class Shot:
    name: str
    pose: Pose


def _orbit(lat: float, lon: float, alt: float, radius_m: float, period_s: float) -> Callable[[float], Pose]:
    def pose_at(t: float) -> Pose:
        ang = 2.0 * math.pi * t / period_s
        dlat = (radius_m * math.cos(ang)) / 111_320.0
        dlon = (radius_m * math.sin(ang)) / (111_320.0 * math.cos(math.radians(lat)))
        heading = (math.degrees(ang) + 90.0) % 360.0      # tangent to the circle
        return Pose(lat + dlat, lon + dlon, alt, yaw=heading, gimbal_yaw=-90.0, gimbal_pitch=-35.0)
    return pose_at


def _slew(t: float) -> Pose:
    # Peak yaw rate = 90 * 2*pi/10 ~ 56.5 deg/s; pitch adds up to ~18 deg/s.
    return Pose(BASE_LAT, BASE_LON, 3000.0, yaw=0.0,
                gimbal_yaw=90.0 * math.sin(2.0 * math.pi * t / 10.0),
                gimbal_pitch=-35.0 + 20.0 * math.sin(2.0 * math.pi * t / 7.0))


def _low_pass(t: float) -> Pose:
    # 100 m/s due east at 600 m HAE from the west side of the city across the bay.
    start_lon = BASE_LON - 0.05
    dlon = (100.0 * t) / (111_320.0 * math.cos(math.radians(BASE_LAT)))
    return Pose(BASE_LAT, start_lon + dlon, 600.0, yaw=90.0, gimbal_pitch=-20.0)


def build_phases(smoke: bool = False) -> list[Phase]:
    if smoke:
        return [Phase("orbit", 20.0, _orbit(BASE_LAT, BASE_LON, 3000.0, 2000.0, 120.0))]
    orbit = _orbit(BASE_LAT, BASE_LON, 3000.0, 2000.0, 120.0)
    far = _orbit(BASE_LAT, FAR_LON, 3000.0, 2000.0, 120.0)

    def warmup(t: float) -> Pose:
        # Visit every measured area once so Cesium's disk cache is warm.
        if t < 60.0:
            return orbit(t * 2.0)
        if t < 90.0:
            return _low_pass((t - 60.0) * 3.0)
        return far(t - 90.0)

    return [
        Phase("warmup", 120.0, warmup, measured=False),
        Phase("orbit", 120.0, orbit),
        Phase("slew", 60.0, _slew),
        Phase("low_pass", 90.0, _low_pass),
        Phase("far_origin", 90.0, far),
    ]


def build_shots(smoke: bool = False) -> list[Shot]:
    nadir = Pose(BASE_LAT, BASE_LON, 3000.0, gimbal_pitch=-90.0)
    if smoke:
        return [Shot("nadir_3km", nadir)]
    slant = Pose(BASE_LAT - 0.06, BASE_LON, 3000.0, yaw=0.0, gimbal_pitch=-17.0)   # ~10 km slant
    horizon = Pose(BASE_LAT, BASE_LON, 1500.0, yaw=270.0, gimbal_pitch=-2.0)
    low_oblique = Pose(BASE_LAT, BASE_LON - 0.03, 400.0, yaw=90.0, gimbal_pitch=-12.0)
    far_slant = replace(slant, lon=FAR_LON)
    night = replace(slant, utc_hour=4, utc_minute=40, day=22)  # ~21:40 PDT, sun ~10 deg below the horizon
    shots = [
        Shot("nadir_3km", nadir),
        Shot("slant_10km", slant),
        Shot("horizon", horizon),
        Shot("low_oblique", low_oblique),
        Shot("dawn_slant", replace(slant, utc_hour=13, utc_minute=15)),          # ~06:15 PDT
        Shot("dusk_slant", replace(slant, utc_hour=3, utc_minute=15, day=22)),   # ~20:15 PDT
        Shot("far_origin_slant", far_slant),
        Shot("far_origin_nadir", replace(nadir, lon=FAR_LON)),
        Shot("night_slant", night),
    ]
    by_name = {s.name: s.pose for s in shots}
    for base in ("nadir_3km", "dusk_slant", "night_slant"):
        shots.append(Shot(f"{base}_ir", replace(by_name[base], sensor_id=1)))
    return shots


def host_datagram(frame_ctr: int, pose: Pose, entity_id: int = CAMERA_ENTITY_ID) -> bytes:
    celestial = {"hour": pose.utc_hour, "minute": pose.utc_minute,
                 "month": pose.month, "day": pose.day, "year": 2026}
    dgram = sc.build_host_frame(frame_ctr, entity_id, pose.lat, pose.lon, pose.alt,
                                pose.yaw, pose.pitch, pose.roll, fov_h=pose.fov_h,
                                sensor_id=pose.sensor_id, celestial=celestial)
    return dgram + sc.pack_art_part_control(entity_id, GIMBAL_ART_PART_ID,
                                            pitch=pose.gimbal_pitch, yaw=pose.gimbal_yaw)
