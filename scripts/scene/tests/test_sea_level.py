import pytest
from fakes import LA_JOLLA, coops_http

from camsim_scene import sea_level


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
