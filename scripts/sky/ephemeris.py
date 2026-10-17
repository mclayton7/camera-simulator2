# Copyright CamSim Contributors. All Rights Reserved.
"""Python mirror of CamSim's FEphemeris (Environment/Ephemeris.cpp) and FNightIllumination (NightIllumination.cpp).

Low-precision Meeus: sun ch. 25, moon ch. 47 (tables 47.A/47.B), topocentric parallax ch. 40, phase ch. 48, sidereal
time ch. 12, ΔT Espenak–Meeus. Expression for expression the C++ code, so scripts/night_check.py points the camera
where CamSim draws the moon (the C++ code's own accuracy vs JPL is held by CamSim.Environment.Ephemeris.*: moon 0.2°,
sun 0.05°). Az/el are geometric (no refraction), azimuth from north through east. Standard library only.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import datetime, timezone

AU_KM = 149597870.7
EARTH_RADIUS_KM = 6378.14
EARTH_B_OVER_A = 0.99664719
SUN_SOLAR_LUX = 127500.0
EXTINCTION_KV = 0.172
HORIZON_CUTOFF_DEG = -0.5

# Meeus table 47.A: (D, M, M', F, Σl 1e-6 deg, Σr 1e-3 km)
MOON_LR = [
    (0, 0, 1, 0, 6288774, -20905355), (2, 0, -1, 0, 1274027, -3699111), (2, 0, 0, 0, 658314, -2955968),
    (0, 0, 2, 0, 213618, -569925), (0, 1, 0, 0, -185116, 48888), (0, 0, 0, 2, -114332, -3149),
    (2, 0, -2, 0, 58793, 246158), (2, -1, -1, 0, 57066, -152138), (2, 0, 1, 0, 53322, -170733),
    (2, -1, 0, 0, 45758, -204586), (0, 1, -1, 0, -40923, -129620), (1, 0, 0, 0, -34720, 108743),
    (0, 1, 1, 0, -30383, 104755), (2, 0, 0, -2, 15327, 10321), (0, 0, 1, 2, -12528, 0),
    (0, 0, 1, -2, 10980, 79661), (4, 0, -1, 0, 10675, -34782), (0, 0, 3, 0, 10034, -23210),
    (4, 0, -2, 0, 8548, -21636), (2, 1, -1, 0, -7888, 24208), (2, 1, 0, 0, -6766, 30824),
    (1, 0, -1, 0, -5163, -8379), (1, 1, 0, 0, 4987, -16675), (2, -1, 1, 0, 4036, -12831),
    (2, 0, 2, 0, 3994, -10445), (4, 0, 0, 0, 3861, -11650), (2, 0, -3, 0, 3665, 14403),
    (0, 1, -2, 0, -2689, -7003), (2, 0, -1, 2, -2602, 0), (2, -1, -2, 0, 2390, 10056),
    (1, 0, 1, 0, -2348, 6322), (2, -2, 0, 0, 2236, -9884), (0, 1, 2, 0, -2120, 5751),
    (0, 2, 0, 0, -2069, 0), (2, -2, -1, 0, 2048, -4950), (2, 0, 1, -2, -1773, 4130),
    (2, 0, 0, 2, -1595, 0), (4, -1, -1, 0, 1215, -3958), (0, 0, 2, 2, -1110, 0),
    (3, 0, -1, 0, -892, 3258), (2, 1, 1, 0, -810, 2616), (4, -1, -2, 0, 759, -1897),
    (0, 2, -1, 0, -713, -2117), (2, 2, -1, 0, -700, 2354), (2, 1, -2, 0, 691, 0),
    (2, -1, 0, -2, 596, 0), (4, 0, 1, 0, 549, -1423), (0, 0, 4, 0, 537, -1117),
    (4, -1, 0, 0, 520, -1571), (1, 0, -2, 0, -487, -1739), (2, 1, 0, -2, -399, 0),
    (0, 0, 2, -2, -381, -4421), (1, 1, 1, 0, 351, 0), (3, 0, -2, 0, -340, 0),
    (4, 0, -3, 0, 330, 0), (2, -1, 2, 0, 327, 0), (0, 2, 1, 0, -323, 1165),
    (1, 1, -1, 0, 299, 0), (2, 0, 3, 0, 294, 0), (2, 0, -1, -2, 0, 8752),
]
# Meeus table 47.B: (D, M, M', F, Σb 1e-6 deg)
MOON_B = [
    (0, 0, 0, 1, 5128122), (0, 0, 1, 1, 280602), (0, 0, 1, -1, 277693), (2, 0, 0, -1, 173237),
    (2, 0, -1, 1, 55413), (2, 0, -1, -1, 46271), (2, 0, 0, 1, 32573), (0, 0, 2, 1, 17198),
    (2, 0, 1, -1, 9266), (0, 0, 2, -1, 8822), (2, -1, 0, -1, 8216), (2, 0, -2, -1, 4324),
    (2, 0, 1, 1, 4200), (2, 1, 0, -1, -3359), (2, -1, -1, 1, 2463), (2, -1, 0, 1, 2211),
    (2, -1, -1, -1, 2065), (0, 1, -1, -1, -1870), (4, 0, -1, -1, 1828), (0, 1, 0, 1, -1794),
    (0, 0, 0, 3, -1749), (0, 1, -1, 1, -1565), (1, 0, 0, 1, -1491), (0, 1, 1, 1, -1475),
    (0, 1, 1, -1, -1410), (0, 1, 0, -1, -1344), (1, 0, 0, -1, -1335), (0, 0, 3, 1, 1107),
    (4, 0, 0, -1, 1021), (4, 0, -1, 1, 833), (0, 0, 1, -3, 777), (4, 0, -2, 1, 671),
    (2, 0, 0, -3, 607), (2, 0, 2, -1, 596), (2, -1, 1, -1, 491), (2, 0, -2, 1, -451),
    (0, 0, 3, -1, 439), (2, 0, 2, 1, 422), (2, 0, -3, -1, 421), (2, 1, -1, 1, -366),
    (2, 1, 0, 1, -351), (4, 0, 0, 1, 331), (2, -1, 1, 1, 315), (2, -2, 0, -1, 302),
    (0, 0, 1, 3, -283), (2, 1, 1, -1, -229), (1, 1, 0, -1, 223), (1, 1, 0, 1, 223),
    (0, 1, -2, -1, -220), (2, 1, -1, -1, -220), (1, 0, 1, 1, -185), (2, -1, -2, -1, 181),
    (0, 1, 2, 1, -177), (4, 0, -2, -1, 176), (4, -1, -1, -1, 166), (1, 0, 1, -1, -164),
    (4, 0, 1, -1, 132), (1, 0, -1, -1, -119), (4, -1, 0, -1, 115), (2, -2, 0, 1, 107),
]
assert len(MOON_LR) == 60 and len(MOON_B) == 60


def _sin(d: float) -> float:
    return math.sin(math.radians(d))


def _cos(d: float) -> float:
    return math.cos(math.radians(d))


def _norm360(d: float) -> float:
    r = math.fmod(d, 360.0)
    return r + 360.0 if r < 0.0 else r


@dataclass(frozen=True)
class Ephem:
    sun_az: float
    sun_el: float
    moon_az: float
    moon_el: float
    moon_phase_angle: float   # 0 = full, 180 = new
    moon_illum: float
    moon_km: float
    sun_au: float
    last_deg: float           # local apparent sidereal time

    @property
    def sun_enu(self) -> tuple[float, float, float]:
        return az_el_to_enu(self.sun_az, self.sun_el)

    @property
    def moon_enu(self) -> tuple[float, float, float]:
        return az_el_to_enu(self.moon_az, self.moon_el)


def az_el_to_enu(az: float, el: float) -> tuple[float, float, float]:
    return (_sin(az) * _cos(el), _cos(az) * _cos(el), _sin(el))


def julian_day(utc: datetime) -> float:
    if utc.tzinfo is None:
        utc = utc.replace(tzinfo=timezone.utc)
    return 2440587.5 + utc.timestamp() / 86400.0


def delta_t_seconds(year: int, month: int) -> float:
    y = min(max(year + (month - 0.5) / 12.0, 1986.0), 2050.0)
    t = y - 2000.0
    if y >= 2005.0:
        return 62.92 + 0.32217 * t + 0.005589 * t * t
    return 63.86 + 0.3345 * t - 0.060374 * t**2 + 0.0017275 * t**3 + 0.000651814 * t**4 + 0.00002373599 * t**5


def _ecl_to_eq(lam: float, beta: float, eps: float) -> tuple[float, float]:
    ra = _norm360(math.degrees(math.atan2(_sin(lam) * _cos(eps) * _cos(beta) - _sin(beta) * _sin(eps),
                                          _cos(lam) * _cos(beta))))
    dec = math.degrees(math.asin(max(-1.0, min(1.0, _sin(beta) * _cos(eps) + _cos(beta) * _sin(eps) * _sin(lam)))))
    return ra, dec


def _eq_to_hor(ha: float, dec: float, lat: float) -> tuple[float, float]:
    el = math.degrees(math.asin(max(-1.0, min(1.0, _sin(lat) * _sin(dec) + _cos(lat) * _cos(dec) * _cos(ha)))))
    az = _norm360(math.degrees(math.atan2(-_sin(ha) * _cos(dec), _cos(lat) * _sin(dec) - _sin(lat) * _cos(dec) * _cos(ha))))
    return az, el


def compute(utc: datetime, lat: float, lon: float, height_m: float = 0.0) -> Ephem:
    jd_ut = julian_day(utc)
    jde = jd_ut + delta_t_seconds(utc.year, utc.month) / 86400.0
    t = (jde - 2451545.0) / 36525.0
    t2, t3, t4 = t * t, t**3, t**4

    omega = 125.04452 - 1934.136261 * t
    l_sun = 280.4665 + 36000.7698 * t
    l_moon = 218.3165 + 481267.8813 * t
    dpsi = (-17.20 * _sin(omega) - 1.32 * _sin(2 * l_sun) - 0.23 * _sin(2 * l_moon) + 0.21 * _sin(2 * omega)) / 3600.0
    deps = (9.20 * _cos(omega) + 0.57 * _cos(2 * l_sun) + 0.10 * _cos(2 * l_moon) - 0.09 * _cos(2 * omega)) / 3600.0
    eps0 = 23.0 + 26.0 / 60.0 + 21.448 / 3600.0 - 46.8150 / 3600.0 * t
    eps_true = eps0 + deps

    # Sun
    l0 = 280.46646 + 36000.76983 * t + 0.0003032 * t2
    m = 357.52911 + 35999.05029 * t - 0.0001537 * t2
    ecc = 0.016708634 - 0.000042037 * t
    c = ((1.914602 - 0.004817 * t - 0.000014 * t2) * _sin(m) + (0.019993 - 0.000101 * t) * _sin(2 * m)
         + 0.000289 * _sin(3 * m))
    nu = m + c
    sun_au = 1.000001018 * (1.0 - ecc * ecc) / (1.0 + ecc * _cos(nu))
    om_s = 125.04 - 1934.136 * t
    sun_ra, sun_dec = _ecl_to_eq(l0 + c - 0.00569 - 0.00478 * _sin(om_s), 0.0, eps0 + 0.00256 * _cos(om_s))

    # Moon (geocentric)
    lp = 218.3164477 + 481267.88123421 * t - 0.0015786 * t2 + t3 / 538841.0 - t4 / 65194000.0
    d = 297.8501921 + 445267.1114034 * t - 0.0018819 * t2 + t3 / 545868.0 - t4 / 113065000.0
    mm = 357.5291092 + 35999.0502909 * t - 0.0001536 * t2 + t3 / 24490000.0
    mp = 134.9633964 + 477198.8675055 * t + 0.0087414 * t2 + t3 / 69699.0 - t4 / 14712000.0
    f = 93.2720950 + 483202.0175233 * t - 0.0036539 * t2 - t3 / 3526000.0 + t4 / 863310000.0
    a1 = 119.75 + 131.849 * t
    a2 = 53.09 + 479264.290 * t
    a3 = 313.45 + 481266.484 * t
    e = 1.0 - 0.002516 * t - 0.0000074 * t2

    def ef(mc: int) -> float:
        return e if abs(mc) == 1 else (e * e if abs(mc) == 2 else 1.0)

    sl = sr = sb = 0.0
    for td, tm, tmp, tf, cl, cr in MOON_LR:
        arg = td * d + tm * mm + tmp * mp + tf * f
        sl += cl * ef(tm) * _sin(arg)
        sr += cr * ef(tm) * _cos(arg)
    for td, tm, tmp, tf, cb in MOON_B:
        sb += cb * ef(tm) * _sin(td * d + tm * mm + tmp * mp + tf * f)
    sl += 3958.0 * _sin(a1) + 1962.0 * _sin(lp - f) + 318.0 * _sin(a2)
    sb += (-2235.0 * _sin(lp) + 382.0 * _sin(a3) + 175.0 * _sin(a1 - f) + 175.0 * _sin(a1 + f)
           + 127.0 * _sin(lp - mp) - 115.0 * _sin(lp + mp))
    moon_km = 385000.56 + sr / 1000.0
    moon_ra, moon_dec = _ecl_to_eq(lp + sl / 1e6 + dpsi, sb / 1e6, eps_true)

    # Local apparent sidereal time (UT1 = UTC)
    tu = (jd_ut - 2451545.0) / 36525.0
    theta0 = 280.46061837 + 360.98564736629 * (jd_ut - 2451545.0) + 0.000387933 * tu * tu - tu**3 / 38710000.0
    last = _norm360(theta0 + dpsi * _cos(eps_true) + lon)

    sun_az, sun_el = _eq_to_hor(last - sun_ra, sun_dec, lat)

    # Moon topocentric as a vector difference (Meeus ch. 40)
    u = math.atan2(EARTH_B_OVER_A * _sin(lat), _cos(lat))
    h_ratio = height_m / (EARTH_RADIUS_KM * 1000.0)
    rho_sin = EARTH_B_OVER_A * math.sin(u) + h_ratio * _sin(lat)
    rho_cos = math.cos(u) + h_ratio * _cos(lat)
    obs = (EARTH_RADIUS_KM * rho_cos * _cos(last), EARTH_RADIUS_KM * rho_cos * _sin(last), EARTH_RADIUS_KM * rho_sin)
    geo = (moon_km * _cos(moon_dec) * _cos(moon_ra), moon_km * _cos(moon_dec) * _sin(moon_ra), moon_km * _sin(moon_dec))
    topo = tuple(g - o for g, o in zip(geo, obs))
    topo_km = math.sqrt(sum(x * x for x in topo))
    topo_ra = math.degrees(math.atan2(topo[1], topo[0]))
    topo_dec = math.degrees(math.asin(max(-1.0, min(1.0, topo[2] / topo_km))))
    moon_az, moon_el = _eq_to_hor(last - topo_ra, topo_dec, lat)

    # Phase (geocentric)
    cos_psi = _sin(sun_dec) * _sin(moon_dec) + _cos(sun_dec) * _cos(moon_dec) * _cos(sun_ra - moon_ra)
    psi = math.acos(max(-1.0, min(1.0, cos_psi)))
    sun_km = sun_au * AU_KM
    i = math.atan2(sun_km * math.sin(psi), moon_km - sun_km * math.cos(psi))
    return Ephem(sun_az, sun_el, moon_az, moon_el, math.degrees(i), (1.0 + math.cos(i)) * 0.5, topo_km, sun_au, last)


# --- FNightIllumination ---

def kasten_young_air_mass(el: float) -> float:
    if el <= HORIZON_CUTOFF_DEG:
        return 40.0
    return 1.0 / (_sin(el) + 0.50572 * (el + 6.07995) ** -1.6364)


def transmittance(el: float) -> float:
    if el < HORIZON_CUTOFF_DEG:
        return 0.0
    return 10.0 ** (-0.4 * EXTINCTION_KV * kasten_young_air_mass(el))


def moon_toa_lux(phase_angle: float, distance_km: float) -> float:
    a = abs(phase_angle)
    mag = 3.84 + 0.026 * a + 4e-9 * a**4
    ratio = 384400.0 / max(distance_km, 1.0)
    return 10.0 ** (-0.4 * mag) * 10.764 * ratio * ratio


def cloud_factor(cover01: float) -> float:
    return 1.0 - 0.75 * min(max(cover01, 0.0), 1.0) ** 3.4


@dataclass(frozen=True)
class Illum:
    sun_ground_lux: float
    moon_ground_lux: float
    floor_ground_lux: float   # the dome floor's horizontal illuminance (E = pi L, L = floor lux x cloud / pi)

    @property
    def total(self) -> float:
        return self.sun_ground_lux + self.moon_ground_lux + self.floor_ground_lux


def illumination(ep: Ephem, cloud01: float = 0.0, night_floor_lux: float = 0.002) -> Illum:
    cf = cloud_factor(cloud01)
    sun = SUN_SOLAR_LUX / max(ep.sun_au**2, 1e-6) * transmittance(ep.sun_el) * max(_sin(ep.sun_el), 0.0) * cf
    moon = moon_toa_lux(ep.moon_phase_angle, ep.moon_km) * transmittance(ep.moon_el) * max(_sin(ep.moon_el), 0.0) * cf
    return Illum(sun, moon, night_floor_lux * cf)


# --- FMoonDisc ---

def moon_shade(u: float, v: float, phase_angle: float) -> float:
    """Lambert shading of the disc point (u, v) in [-1, 1]^2, u toward the sun's side (FMoonDisc::Shade)."""
    r2 = u * u + v * v
    if r2 > 1.0:
        return 0.0
    a = math.radians(phase_angle)
    return max(u * math.sin(a) + math.sqrt(1.0 - r2) * math.cos(a), 0.0)
