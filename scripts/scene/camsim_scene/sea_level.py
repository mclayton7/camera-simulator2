"""Local mean sea level for a scene package (REALISM R1): CamSim's tide-0 sea is EGM96, local MSL is not. `plan`
snapshots a NOAA CO-OPS station's tidal datums into the manifest (`sea_level`); `build` turns them into
sea_level.json (offset_m = local MSL - EGM96 at the station), which CamSim adds under the CIGI tide."""

from __future__ import annotations

import math
from pathlib import Path

from .datum import GRID_URL

COOPS = "https://api.tidesandcurrents.noaa.gov/mdapi/prod/webapi/stations/{station}"
COOPS_DATUMS = COOPS + "/datums.json?units=metric"
COOPS_STATION = COOPS + ".json"
GEOID_DATUM = "nad83_2011_navd88_geoid18"
EGM96_GRID = "us_nga_egm96_15.tif"
FILE = "sea_level.json"
MAX_OFFSET_M = 3.0  # CamSim's MaxSeaLevelOffsetM: a larger |offset| is a resolve error at launch


class SeaLevelError(Exception):
    pass


def station_section(http, station: str) -> dict:
    """The manifest's `sea_level` section from CO-OPS (values as published, metres, station datum)."""
    try:
        return _station_section(http, station)
    except (KeyError, TypeError, ValueError, AttributeError) as e:
        raise SeaLevelError(f"CO-OPS station {station}: unexpected response ({type(e).__name__}: {e})") from e


def _station_section(http, station: str) -> dict:
    d = http.get_json(COOPS_DATUMS.format(station=station))
    values = {x["name"]: x["value"] for x in d.get("datums") or [] if x.get("value") is not None}
    missing = [k for k in ("MSL", "NAVD88") if k not in values]
    if missing:
        raise SeaLevelError(
            f"CO-OPS station {station} publishes no {' or '.join(missing)} datum: pick a station with a NAVD88 tie"
        )
    st = (http.get_json(COOPS_STATION.format(station=station)).get("stations") or [{}])[0]
    return {
        "station": station,
        "name": st.get("name", ""),
        "lat": float(st["lat"]),
        "lon": float(st["lng"]),
        "epoch": d.get("epoch", ""),
        "msl_m": float(values["MSL"]),
        "navd88_m": float(values["NAVD88"]),
        "msl_above_navd88_m": round(float(values["MSL"]) - float(values["NAVD88"]), 6),
        "geoid": GEOID_DATUM,
        "egm96_grid": {"name": EGM96_GRID, "url": GRID_URL.format(name=EGM96_GRID), "sha256": None},
    }


def compute(section: dict, grid_paths: dict) -> dict:
    """offset_m = h_ell(NAVD88 0) + (MSL - NAVD88) - N_EGM96 at the station (PROJ network off, pinned grids).
    N_EGM96 is bilinear on NGA's 15' grid, as CamSim's Geospatial/Geoid.cpp reads WW15MGH.DAC."""
    import numpy as np

    from .datum import DatumTransform, manifest_section
    from .sources.base import SourceRaster

    lon, lat = np.array([section["lon"]]), np.array([section["lat"]])
    entry = manifest_section([section["geoid"]])["datums"][section["geoid"]]
    dt = DatumTransform(entry, {k: Path(v) for k, v in grid_paths.items()})
    h0 = float(dt.vertical_offset(*dt.to_source_geographic(lon, lat))[0])
    egm = SourceRaster(path=Path(grid_paths[EGM96_GRID]), datum="wgs84", clamp_edges=True)
    vals, ok = egm.sample(lon, lat, 0.0)
    if not ok[0]:
        raise SeaLevelError(f"EGM96 grid has no value at {section['lat']}, {section['lon']}")
    n = float(vals[0][0])
    offset = round(h0 + section["msl_above_navd88_m"] - n, 4)
    if not math.isfinite(offset) or abs(offset) > MAX_OFFSET_M:
        raise SeaLevelError(
            f"sea level offset {offset} m at station {section['station']} is not finite or outside "
            f"+/-{MAX_OFFSET_M:g} m (CamSim rejects it): check the station's datums and the pinned grids"
        )
    return {
        "station": section["station"],
        "name": section.get("name", ""),
        "msl_above_navd88_m": section["msl_above_navd88_m"],
        "navd88_ellipsoid_m": round(h0, 4),
        "egm96_n_m": round(n, 4),
        "offset_m": offset,
    }


def write(pkg: Path, m, grid_paths: dict) -> dict | None:
    """Write <pkg>/sea_level.json when the manifest has a station; remove a stale one otherwise."""
    from .fsutil import atomic_write
    from .manifest import canonical_json

    p = Path(pkg) / FILE
    if not m.sea_level:
        p.unlink(missing_ok=True)
        return None
    r = compute(m.sea_level, grid_paths)
    atomic_write(p, canonical_json(r).encode())
    return r
