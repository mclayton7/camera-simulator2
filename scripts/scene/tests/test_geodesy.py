import numpy as np

from camsim_scene import geodesy


def test_ecef_round_trip_sub_mm():
    lon = np.array([-117.45, 0.0, 179.9, -45.0])
    lat = np.array([33.30, 0.0, -60.0, 89.0])
    h = np.array([120.5, -30.0, 4000.0, 0.0])
    lo, la, hh = geodesy.ecef_to_geodetic(geodesy.geodetic_to_ecef(lon, lat, h))
    assert np.allclose(lo, lon, atol=1e-10) and np.allclose(la, lat, atol=1e-10) and np.allclose(hh, h, atol=1e-4)


def test_equator_prime_meridian_is_semi_major_axis():
    assert np.allclose(geodesy.geodetic_to_ecef(0.0, 0.0, 0.0), [6378137.0, 0.0, 0.0])


def test_enu_matrix_maps_local_up_to_height():
    m = geodesy.enu_to_ecef_matrix(-117.45, 33.30, 100.0)
    p = m @ np.array([0.0, 0.0, 10.0, 1.0])
    _, _, h = geodesy.ecef_to_geodetic(p[:3])
    assert abs(h - 110.0) < 1e-6
    v = geodesy.matrix_to_column_major(m)
    assert len(v) == 16 and np.allclose(geodesy.column_major_to_matrix(v), m)


def test_enu_axes_point_east_north_up():
    lon0, lat0 = -117.45, 33.30
    m = geodesy.enu_to_ecef_matrix(lon0, lat0, 0.0)
    assert np.allclose(geodesy.enu_rotation(lon0, lat0) @ geodesy.enu_rotation(lon0, lat0).T, np.eye(3))
    for local, axis in (((100.0, 0, 0), 0), ((0, 100.0, 0), 1)):
        lo, la, _ = geodesy.ecef_to_geodetic((m @ np.array([*local, 1.0]))[:3])
        d = (lo - lon0, la - lat0)
        assert d[axis] > 0 and abs(d[1 - axis]) < 1e-6
