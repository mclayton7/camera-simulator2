"""Sun position for NAIP acquisition metadata (REALISM R1 chunk 2): NOAA Global Monitoring Laboratory solar
calculator equations (after Meeus, "Astronomical Algorithms"). Geometric elevation: no refraction. The date is all
NAIP gives (its times are a placeholder), so these are bounds: the sun at local solar noon, and its azimuths when it
crosses NAIP's minimum flying elevation."""

from __future__ import annotations

import datetime as dt
import math

NAIP_MIN_ELEVATION_DEG = 30.0


def _declination_and_eot(jd: float) -> tuple[float, float]:
    """Solar declination (degrees) and equation of time (minutes) at Julian day jd."""
    t = (jd - 2451545.0) / 36525.0
    l0 = (280.46646 + t * (36000.76983 + t * 0.0003032)) % 360.0
    m = 357.52911 + t * (35999.05029 - 0.0001537 * t)
    e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t)
    mr = math.radians(m)
    c = (
        math.sin(mr) * (1.914602 - t * (0.004817 + 0.000014 * t))
        + math.sin(2 * mr) * (0.019993 - 0.000101 * t)
        + math.sin(3 * mr) * 0.000289
    )
    omega = 125.04 - 1934.136 * t
    lam = l0 + c - 0.00569 - 0.00478 * math.sin(math.radians(omega))
    eps0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0
    eps = eps0 + 0.00256 * math.cos(math.radians(omega))
    decl = math.degrees(math.asin(math.sin(math.radians(eps)) * math.sin(math.radians(lam))))
    y = math.tan(math.radians(eps / 2)) ** 2
    l0r = math.radians(l0)
    eot = 4.0 * math.degrees(
        y * math.sin(2 * l0r)
        - 2 * e * math.sin(mr)
        + 4 * e * y * math.sin(mr) * math.cos(2 * l0r)
        - 0.5 * y * y * math.sin(4 * l0r)
        - 1.25 * e * e * math.sin(2 * mr)
    )
    return decl, eot


def _julian_day(d: dt.date, minutes_utc: float) -> float:
    return d.toordinal() + 1721424.5 + minutes_utc / 1440.0


def _noon_declination(date: dt.date, lon: float) -> float:
    """The declination at local solar noon (which depends on the equation of time there: three steps converge)."""
    minutes = 720.0
    for _ in range(3):
        _, eot = _declination_and_eot(_julian_day(date, minutes))
        minutes = 720.0 - 4.0 * lon - eot
    return _declination_and_eot(_julian_day(date, minutes))[0]


def solar_noon(date: dt.date, lon: float, lat: float) -> dict:
    """Sun elevation and azimuth (degrees from north, rounded to 0.01) at local solar noon on `date` at (lon, lat)."""
    decl = _noon_declination(date, lon)
    return {"elevation_deg": round(90.0 - abs(lat - decl), 2), "azimuth_deg": 180.0 if lat >= decl else 0.0}


def flight_window(date: dt.date, lon: float, lat: float, min_elevation: float = NAIP_MIN_ELEVATION_DEG) -> dict | None:
    """The sun's azimuths (degrees from north, rounded to 0.01) when it crosses `min_elevation` in the morning and
    the afternoon of `date` (declination at solar noon); None when the noon sun is below it."""
    decl = _noon_declination(date, lon)
    if 90.0 - abs(lat - decl) < min_elevation:
        return None
    p, d, h0 = math.radians(lat), math.radians(decl), math.radians(min_elevation)
    cos_h = (math.sin(h0) - math.sin(p) * math.sin(d)) / (math.cos(p) * math.cos(d))
    h = math.acos(max(-1.0, min(1.0, cos_h)))
    az = math.degrees(math.atan2(math.sin(h), math.cos(h) * math.sin(p) - math.tan(d) * math.cos(p))) + 180.0
    return {"min_elevation_deg": min_elevation, "azimuth_deg": [round(360.0 - az, 2), round(az, 2)]}
