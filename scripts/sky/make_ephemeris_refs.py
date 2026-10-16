# Copyright CamSim Contributors. All Rights Reserved.
"""Reference sun/moon positions for CamSim.Environment.Ephemeris.* (JPL DE421 via skyfield).

uv run --with skyfield python scripts/sky/make_ephemeris_refs.py
Prints C++ initialiser rows: {Y,M,D,h,m, lat, lon, sunAz, sunEl, moonAz, moonEl, illum, moonKm, lastDeg}.
"""
from skyfield.api import load, wgs84
from skyfield import almanac

CASES = [
    # (UTC y, m, d, h, mi, lat, lon) — Pendleton, SF, Eglin, Yuma, polar, southern, lunar standstill (2025-01), full/new/quarter moons
    (2026, 10, 16, 6, 0, 33.30, -117.35), (2026, 10, 26, 7, 0, 33.30, -117.35),
    (2026, 11, 24, 8, 0, 37.77, -122.42), (2026, 12, 1, 4, 30, 30.46, -86.55),
    (2027, 3, 22, 10, 0, 32.65, -114.60), (2026, 6, 21, 12, 0, 78.22, 15.65),
    (2026, 12, 21, 0, 0, -33.87, 151.21), (2025, 1, 13, 22, 0, 33.30, -117.35),
    (2026, 9, 26, 3, 0, 51.48, 0.0), (2026, 7, 4, 2, 15, 64.84, -147.72),
    (2024, 4, 8, 18, 18, 23.0, -105.0),   # total solar eclipse: sun and moon within ~0.1°
]
ts = load.timescale()
eph = load("de421.bsp")
earth, sun, moon = eph["earth"], eph["sun"], eph["moon"]
for (y, mo, d, h, mi, lat, lon) in CASES:
    t = ts.utc(y, mo, d, h, mi)
    obs = earth + wgs84.latlon(lat, lon, 0)
    sa = obs.at(t).observe(sun).apparent().altaz()
    ma = obs.at(t).observe(moon).apparent().altaz()
    illum = almanac.fraction_illuminated(eph, "moon", t)
    last = (t.gast * 15.0 + lon) % 360.0
    print(f"\t{{{y},{mo},{d},{h},{mi}, {lat}, {lon}, {sa[1].degrees:.4f}, {sa[0].degrees:.4f}, "
          f"{ma[1].degrees:.4f}, {ma[0].degrees:.4f}, {illum:.4f}, {ma[2].km:.0f}, {last:.4f}}},")
