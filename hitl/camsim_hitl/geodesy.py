"""WGS-84 / EGM96 helpers: geoid, MSL <-> ellipsoid, lever arm, near-ground blend.

HITL.md "Frames, altitudes and conversions": CamSim takes WGS-84 ellipsoid
heights everywhere, X-Plane reports MSL on a sphere, so

    h_WGS84 = h_MSL + N_EGM96(lat, lon)

using the same 15' grid and bilinear interpolation as CamSim's
Geospatial/Geoid.cpp (and scripts/ocean_check.py, check.js).
"""

from __future__ import annotations

import logging
import math
import struct
from pathlib import Path

from .quat import Quat

log = logging.getLogger(__name__)

WGS84_A = 6378137.0
WGS84_F = 1.0 / 298.257223563
WGS84_E2 = WGS84_F * (2.0 - WGS84_F)

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_GEOID_PATH = (
    REPO_ROOT / "unreal_project/CamSimTest/Content/NonUFS/Geoid/WW15MGH.DAC"
)
_GRID_ROWS, _GRID_COLS = 721, 1440


class Geoid:
    """EGM96 undulation N (m) from the WW15MGH.DAC grid (int16 BE, cm, 90N..90S, 0..360E)."""

    def __init__(self, path: str | Path | None = None, allow_missing: bool = False):
        self.path = Path(path) if path else DEFAULT_GEOID_PATH
        self._grid: bytes | None = None
        try:
            data = self.path.read_bytes()
        except OSError as e:
            if not allow_missing:
                raise FileNotFoundError(
                    f"EGM96 grid not found at {self.path} ({e}); set geodesy.geoid_path"
                ) from e
            log.warning("EGM96 grid missing (%s): N_geoid = 0 everywhere", self.path)
            return
        if len(data) != _GRID_ROWS * _GRID_COLS * 2:
            msg = f"{self.path} is not the EGM96 grid ({len(data)} bytes; git lfs pull?)"
            if not allow_missing:
                raise ValueError(msg)
            log.warning("%s: N_geoid = 0 everywhere", msg)
            return
        self._grid = data

    @property
    def available(self) -> bool:
        return self._grid is not None

    def _at(self, row: int, col: int) -> float:
        assert self._grid is not None
        i = 2 * (row * _GRID_COLS + col % _GRID_COLS)
        return struct.unpack_from(">h", self._grid, i)[0] / 100.0

    def undulation(self, lat: float, lon: float) -> float:
        if self._grid is None:
            return 0.0
        lat = max(-90.0, min(90.0, lat))
        y = (90.0 - lat) / 0.25
        x = (lon % 360.0) / 0.25
        r, c = min(math.floor(y), _GRID_ROWS - 2), math.floor(x)
        fy, fx = y - r, x - c
        return (self._at(r, c) * (1 - fx) + self._at(r, c + 1) * fx) * (1 - fy) + (
            self._at(r + 1, c) * (1 - fx) + self._at(r + 1, c + 1) * fx
        ) * fy

    def msl_to_ellipsoid(self, lat: float, lon: float, h_msl: float) -> float:
        return h_msl + self.undulation(lat, lon)

    def ellipsoid_to_msl(self, lat: float, lon: float, h_ell: float) -> float:
        return h_ell - self.undulation(lat, lon)


def radii(lat_deg: float) -> tuple[float, float]:
    """(M, N): WGS-84 meridian and prime-vertical radii of curvature (m)."""
    s = math.sin(math.radians(lat_deg))
    w = math.sqrt(1.0 - WGS84_E2 * s * s)
    n = WGS84_A / w
    m = WGS84_A * (1.0 - WGS84_E2) / (w * w * w)
    return m, n


def offset_ned(
    lat: float, lon: float, h: float, dn: float, de: float, dd: float
) -> tuple[float, float, float]:
    """Shift a geodetic point by a small NED offset (m). Degrees in and out."""
    m, n = radii(lat)
    lat2 = lat + math.degrees(dn / (m + h))
    coslat = max(1e-9, math.cos(math.radians(lat)))
    lon2 = lon + math.degrees(de / ((n + h) * coslat))
    lon2 = (lon2 + 180.0) % 360.0 - 180.0
    return lat2, lon2, h - dd


def apply_lever_arm(
    lat: float, lon: float, h: float, q_nb: Quat, r_cam_body: tuple[float, float, float]
) -> tuple[float, float, float]:
    """HITL.md "Camera lever arm": [dN, dE, dD] = R_NB r_cam, then shift the position."""
    if not any(r_cam_body):
        return lat, lon, h
    dn, de, dd = q_nb.rotate(r_cam_body)
    return offset_ned(lat, lon, h, dn, de, dd)


def ned_distance(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Horizontal distance (m) between nearby points, local-tangent approximation."""
    m, n = radii(lat1)
    dn = math.radians(lat2 - lat1) * m
    dlon = (lon2 - lon1 + 180.0) % 360.0 - 180.0
    de = math.radians(dlon) * n * math.cos(math.radians(lat1))
    return math.hypot(dn, de)


def smoothstep(e0: float, e1: float, x: float) -> float:
    if e1 <= e0:
        return 1.0 if x < e0 else 0.0
    t = max(0.0, min(1.0, (x - e0) / (e1 - e0)))
    return t * t * (3.0 - 2.0 * t)


class TerrainBlend:
    """Near-ground altitude blend (HITL.md "Terrain near the ground").

    Below ``band_low_m`` AGL the altitude is ``HOT_Cesium + AGL`` so the wheels
    touch the rendered runway; above ``band_high_m`` it is ``elevation + N``;
    in between a smoothstep weight. The blend is applied as a correction
    c = (HOT + AGL) - (elevation + N), low-passed with ``tau_s`` so 1-5 Hz HOT
    updates never step the camera; it decays to 0 when HOT is missing, stale or
    was measured too far from the aircraft.
    """

    def __init__(
        self,
        band_low_m: float = 50.0,
        band_high_m: float = 100.0,
        tau_s: float = 0.5,
        hot_max_age_s: float = 3.0,
        hot_max_distance_m: float = 50.0,
    ):
        self.band_low_m = band_low_m
        self.band_high_m = band_high_m
        self.tau_s = tau_s
        self.hot_max_age_s = hot_max_age_s
        self.hot_max_distance_m = hot_max_distance_m
        self._hot: tuple[float, float, float, float] | None = None  # lat, lon, hot, t
        self._corr = 0.0
        self._t_last: float | None = None

    def in_band(self, agl: float) -> bool:
        """Whether HOT should be polled (a margin above the band so it is ready)."""
        return agl < self.band_high_m * 1.5

    def update_hot(self, lat: float, lon: float, hot_ell: float, t: float) -> None:
        self._hot = (lat, lon, hot_ell, t)

    def weight(self, agl: float) -> float:
        return 1.0 - smoothstep(self.band_low_m, self.band_high_m, agl)

    def altitude(self, lat: float, lon: float, alt_ell: float, agl: float, t: float) -> float:
        """Blended ellipsoid altitude at time t (seconds, monotonic)."""
        target = 0.0
        w = self.weight(agl)
        if self._hot is not None and w > 0.0:
            hlat, hlon, hot, ht = self._hot
            fresh = (t - ht) <= self.hot_max_age_s
            near = ned_distance(lat, lon, hlat, hlon) <= self.hot_max_distance_m
            if fresh and near:
                target = (hot + agl) - alt_ell
        if self._t_last is None:
            self._corr = target
        else:
            dt = max(0.0, t - self._t_last)
            a = 1.0 if self.tau_s <= 0 else 1.0 - math.exp(-dt / self.tau_s)
            self._corr += a * (target - self._corr)
        self._t_last = t
        return alt_ell + w * self._corr

    @property
    def correction(self) -> float:
        return self._corr
