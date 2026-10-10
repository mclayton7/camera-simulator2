"""Local mean sea level for a scene package (REALISM R1): CamSim's tide-0 sea is EGM96, local MSL is not. `plan`
snapshots a NOAA CO-OPS station's tidal datums into the manifest (`sea_level`); `build` turns them into
sea_level.json (offset_m = local MSL - EGM96 at the station), which CamSim adds under the CIGI tide."""

from __future__ import annotations

from .datum import GRID_URL

COOPS = "https://api.tidesandcurrents.noaa.gov/mdapi/prod/webapi/stations/{station}"
COOPS_DATUMS = COOPS + "/datums.json?units=metric"
COOPS_STATION = COOPS + ".json"
GEOID_DATUM = "nad83_2011_navd88_geoid18"
EGM96_GRID = "us_nga_egm96_15.tif"
FILE = "sea_level.json"


class SeaLevelError(Exception):
    pass


def station_section(http, station: str) -> dict:
    """The manifest's `sea_level` section from CO-OPS (values as published, metres, station datum)."""
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
