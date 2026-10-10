from pathlib import Path

import numpy as np
import pytest
from fakes import LA_JOLLA, coops_http

from camsim_scene import sea_level
from camsim_scene.manifest import Manifest
from rasters import write_geotiff

GEOID18 = Path(__file__).parent / "fixtures" / "grids" / "us_noaa_g2018u0.tif"


def test_station_section_from_coops():
    s = sea_level.station_section(coops_http(), "9410230")
    assert s["station"] == "9410230" and s["name"] == "La Jolla"
    assert s["msl_above_navd88_m"] == pytest.approx(0.774, abs=1e-9)
    assert (s["lat"], s["lon"], s["epoch"]) == (32.8669, -117.2571, "1983-2001")
    assert s["egm96_grid"] == {
        "name": "us_nga_egm96_15.tif",
        "url": "https://cdn.proj.org/us_nga_egm96_15.tif",
        "sha256": None,
    }


def test_station_without_navd88_is_an_error():
    no_navd = {**LA_JOLLA, "datums": [d for d in LA_JOLLA["datums"] if d["name"] != "NAVD88"]}
    with pytest.raises(sea_level.SeaLevelError, match="NAVD88"):
        sea_level.station_section(coops_http(no_navd), "9410230")


def egm96_grid(tmp_path, value=-35.2):
    # a constant fake EGM96 grid (15' nodes) around La Jolla
    write_geotiff(tmp_path / "egm96.tif", np.full((40, 40), value, np.float32), -122.0, 37.0, 0.25)
    return tmp_path / "egm96.tif"


def test_compute_offset_at_la_jolla(tmp_path):
    s = sea_level.station_section(coops_http(), "9410230")
    grids = {"us_noaa_g2018u0.tif": GEOID18, sea_level.EGM96_GRID: egm96_grid(tmp_path)}
    r = sea_level.compute(s, grids)
    # NAVD88 zero at La Jolla is ~ -35.6 m ellipsoid height (GEOID18 + ITRF2014 Helmert)
    assert -36.5 < r["navd88_ellipsoid_m"] < -34.5
    assert r["egm96_n_m"] == pytest.approx(-35.2, abs=1e-6)
    assert r["offset_m"] == pytest.approx(r["navd88_ellipsoid_m"] + 0.774 + 35.2, abs=1e-4)


def test_write_removes_a_stale_file_when_there_is_no_station(tmp_path):
    (tmp_path / sea_level.FILE).write_text("{}")
    m = Manifest.__new__(Manifest)
    m.sea_level = None
    assert sea_level.write(tmp_path, m, {}) is None and not (tmp_path / sea_level.FILE).exists()
