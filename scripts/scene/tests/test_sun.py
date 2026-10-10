import datetime as dt

import pytest

from camsim_scene.sun import _declination_and_eot, _julian_day, flight_window, solar_noon


@pytest.mark.parametrize(
    "day, lon, lat, elevation, azimuth",
    [
        (dt.date(2022, 6, 21), -117.4, 33.3, 80.14, 180.0),  # 90 - (33.3 - 23.44)
        (dt.date(2022, 12, 21), -117.4, 33.3, 33.26, 180.0),  # 90 - (33.3 + 23.44)
        (dt.date(2022, 6, 21), 151.2, -33.9, 32.66, 0.0),  # southern winter: the sun is due north
        (dt.date(2022, 6, 21), 0.0, 10.0, 76.56, 0.0),  # tropics, sun north of the zenith
    ],
)
def test_noon_elevation_is_ninety_minus_latitude_minus_declination(day, lon, lat, elevation, azimuth):
    s = solar_noon(day, lon, lat)
    assert s["elevation_deg"] == pytest.approx(elevation, abs=0.05) and s["azimuth_deg"] == azimuth


def test_declination_and_equation_of_time_on_a_naip_flight_day():
    decl, eot = _declination_and_eot(_julian_day(dt.date(2022, 5, 30), 20 * 60))
    assert decl == pytest.approx(21.86, abs=0.1) and eot == pytest.approx(2.4, abs=0.3)  # minutes


def test_flight_window_azimuths():
    eq = flight_window(dt.date(2022, 3, 20), 0.0, 0.0)  # equinox at the equator: due east, then due west
    assert eq["min_elevation_deg"] == 30.0
    assert eq["azimuth_deg"][0] == pytest.approx(90.0, abs=0.5) and eq["azimuth_deg"][1] == pytest.approx(
        270.0, abs=0.5
    )
    summer = flight_window(dt.date(2022, 5, 30), -117.34377, 33.2187485)
    assert summer["azimuth_deg"] == pytest.approx([82.2, 277.8], abs=0.05)
    winter = flight_window(dt.date(2022, 12, 21), -117.4, 33.3)
    assert winter["azimuth_deg"] == pytest.approx([158.24, 201.76], abs=0.05)
    assert flight_window(dt.date(2022, 12, 21), 0.0, 70.0) is None  # the sun never reaches 30 degrees
