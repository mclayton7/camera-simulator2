# Copyright CamSim Contributors. All Rights Reserved.
"""Python mirror of CamSim's moon disc surface (Environment/MoonSurface.cpp, Shaders/Private/CamSimSky.ush
CamSimMoonDisc) for scripts/night_check.py N3: the albedo map (Content/NonUFS/Sky/moon_albedo.pgm), the IAU body
orientation (Archinal et al. 2011) and lunar-Lambert photometry (McEwen 1991/1996), evaluated for whole images.

Frames: everything here works in any orthonormal frame; night_check uses local East-North-Up at the observer. J2000 →
ENU goes through the IAU 1976 precession (FSkyDomeDriver::PrecessionJ2000ToDate) and the local apparent sidereal time.
"""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
ALBEDO_PGM = REPO / "unreal_project/CamSimTest/Content/NonUFS/Sky/moon_albedo.pgm"


def lunar_lambert_l(phase_deg: float) -> float:
    """FMoonDisc::LunarLambertL: McEwen's L(alpha), alpha in degrees, clamped to [0, 1]."""
    a = abs(phase_deg)
    return min(max(1.0 - 0.019 * a + 2.42e-4 * a * a - 1.46e-6 * a ** 3, 0.0), 1.0)


def load_albedo(path: Path = ALBEDO_PGM) -> np.ndarray | None:
    """The 8-bit binary PGM as float [0, 1] (rows from latitude +90), or None if missing / an LFS pointer."""
    if not path.exists():
        return None
    b = path.read_bytes()
    if not b.startswith(b"P5"):
        return None
    tokens, pos = [], 2
    while len(tokens) < 3:
        while b[pos:pos + 1].isspace():
            pos += 1
        if b[pos:pos + 1] == b"#":
            pos = b.index(b"\n", pos) + 1
            continue
        start = pos
        while not b[pos:pos + 1].isspace():
            pos += 1
        tokens.append(int(b[start:pos]))
    w, h, maxval = tokens
    assert maxval == 255
    pos += 1
    return np.frombuffer(b, dtype=np.uint8, count=w * h, offset=pos).reshape(h, w).astype(np.float64) / 255.0


def sample_albedo(img: np.ndarray | None, lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
    """FMoonAlbedoMap::Sample: bilinear between texel centres, longitude wraps, latitude clamps; 1 without a map."""
    if img is None:
        return np.ones_like(lon)
    h, w = img.shape
    x = (lon / (2 * math.pi) + 0.5) * w - 0.5
    y = np.clip((0.5 - lat / math.pi) * h - 0.5, 0.0, h - 1.0)
    x0 = np.floor(x)
    y0 = np.floor(y)
    fx, fy = x - x0, y - y0
    x0i = np.mod(x0.astype(np.int64), w)
    x1i = np.mod(x0i + 1, w)
    y0i = y0.astype(np.int64)
    y1i = np.minimum(y0i + 1, h - 1)
    top = img[y0i, x0i] * (1 - fx) + img[y0i, x1i] * fx
    bot = img[y1i, x0i] * (1 - fx) + img[y1i, x1i] * fx
    return top * (1 - fy) + bot * fy


def iau_axes_j2000(days_tt: float) -> np.ndarray:
    """FMoonOrientation::IauAxesJ2000: rows = body x (lon 0, lat 0), y (lon 90 E), z (north pole) in J2000."""
    d, t = days_tt, days_tt / 36525.0

    def e(a0: float, rate: float) -> float:
        return math.radians(a0 + rate * d)

    e1, e2, e3 = e(125.045, -0.0529921), e(250.089, -0.1059842), e(260.008, 13.0120009)
    e4, e5, e6 = e(176.625, 13.3407154), e(357.529, 0.9856003), e(311.589, 26.4057084)
    e7, e8, e9 = e(134.963, 13.0649930), e(276.617, 0.3287146), e(34.226, 1.7484877)
    e10, e11, e12, e13 = e(15.134, -0.1589763), e(119.743, 0.0036096), e(239.961, 0.1643573), e(25.053, 12.9590088)
    s, c = math.sin, math.cos
    ra = (269.9949 + 0.0031 * t - 3.8787 * s(e1) - 0.1204 * s(e2) + 0.0700 * s(e3) - 0.0172 * s(e4) + 0.0072 * s(e6)
          - 0.0052 * s(e10) + 0.0043 * s(e13))
    dec = (66.5392 + 0.0130 * t + 1.5419 * c(e1) + 0.0239 * c(e2) - 0.0278 * c(e3) + 0.0068 * c(e4) - 0.0029 * c(e6)
           + 0.0009 * c(e7) + 0.0008 * c(e10) - 0.0009 * c(e13))
    w = (38.3213 + 13.17635815 * d - 1.4e-12 * d * d + 3.5610 * s(e1) + 0.1208 * s(e2) - 0.0642 * s(e3) + 0.0158 * s(e4)
         + 0.0252 * s(e5) - 0.0066 * s(e6) - 0.0047 * s(e7) - 0.0046 * s(e8) + 0.0028 * s(e9) + 0.0052 * s(e10)
         + 0.0040 * s(e11) + 0.0019 * s(e12) - 0.0044 * s(e13))
    a, dl, wr = math.radians(ra), math.radians(dec), math.radians(math.fmod(w, 360.0))
    pole = np.array([c(dl) * c(a), c(dl) * s(a), s(dl)])
    node = np.array([-s(a), c(a), 0.0])
    node_east = np.cross(pole, node)
    x = node * c(wr) + node_east * s(wr)
    return np.array([x, np.cross(pole, x), pole])


def precession_j2000_to_date(t: float) -> np.ndarray:
    """FSkyDomeDriver::PrecessionJ2000ToDate (Meeus 21.2/21.4): rows map J2000 to the mean equator of date."""
    k = math.pi / (180.0 * 3600.0)
    zeta = (2306.2181 * t + 0.30188 * t * t + 0.017998 * t ** 3) * k
    z = (2306.2181 * t + 1.09468 * t * t + 0.018203 * t ** 3) * k
    th = (2004.3109 * t - 0.42665 * t * t - 0.041833 * t ** 3) * k

    def rz(a: float) -> np.ndarray:
        return np.array([[math.cos(a), -math.sin(a), 0.0], [math.sin(a), math.cos(a), 0.0], [0.0, 0.0, 1.0]])

    rt = np.array([[math.cos(th), 0.0, -math.sin(th)], [0.0, 1.0, 0.0], [math.sin(th), 0.0, math.cos(th)]])
    return rz(z) @ rt @ rz(zeta)


def j2000_to_enu(lat_deg: float, last_deg: float, centuries: float) -> np.ndarray:
    """Matrix taking a J2000 equatorial vector to local ENU (FSkyDomeDriver::SkyRotationRows' inverse)."""
    p, th = math.radians(lat_deg), math.radians(last_deg)
    east = [-math.sin(th), math.cos(th), 0.0]
    north = [-math.sin(p) * math.cos(th), -math.sin(p) * math.sin(th), math.cos(p)]
    zen = [math.cos(p) * math.cos(th), math.cos(p) * math.sin(th), math.sin(p)]
    return np.array([east, north, zen]) @ precession_j2000_to_date(centuries)


def days_tt(julian_day_utc: float, delta_t_s: float) -> float:
    return julian_day_utc - 2451545.0 + delta_t_s / 86400.0


def moon_right(moon: np.ndarray, sun: np.ndarray) -> np.ndarray:
    """CamSimMoonRight / FMoonDisc::DiscBasis."""
    p = sun - (sun @ moon) * moon
    if p @ p < 1e-12:
        p = np.cross(moon, [0.0, 0.0, 1.0] if abs(moon[2]) < 0.9 else [1.0, 0.0, 0.0])
    return p / np.linalg.norm(p)


def disc_radiance(view: np.ndarray, moon: np.ndarray, sun: np.ndarray, ang_radius: float, lam_l: float,
                  axes: np.ndarray, albedo: np.ndarray | None) -> np.ndarray:
    """CamSimMoonDisc without the luminance scale, for view directions view (..., 3) (unit): albedo x lunar-Lambert."""
    right = moon_right(moon, sun)
    up = np.cross(moon, right)
    u = (view @ right) / math.sin(ang_radius)
    v = (view @ up) / math.sin(ang_radius)
    r2 = u * u + v * v
    inside = ((view @ moon) >= math.cos(ang_radius)) & (r2 <= 1.0)
    nz = np.sqrt(np.clip(1.0 - r2, 0.0, 1.0))
    n = u[..., None] * right + v[..., None] * up - nz[..., None] * moon
    mu0 = n @ sun
    shade = np.where(mu0 > 0, 2.0 * lam_l * mu0 / np.maximum(mu0 + nz, 1e-12) + (1.0 - lam_l) * mu0, 0.0)
    lon = np.arctan2(n @ axes[1], n @ axes[0])
    lat = np.arcsin(np.clip(n @ axes[2], -1.0, 1.0))
    return np.where(inside, sample_albedo(albedo, lon, lat) * shade, 0.0)
