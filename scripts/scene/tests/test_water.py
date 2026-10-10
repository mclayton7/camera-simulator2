import numpy as np
from rasters import write_geotiff

from camsim_scene import water
from camsim_scene.sources.base import SourceRaster


def raster(tmp_path, name, codes, west, north, res=0.001):
    write_geotiff(tmp_path / name, np.asarray(codes, np.uint8), west, north, res, overviews=())
    return SourceRaster(path=tmp_path / name, datum="wgs84")


def test_class_codes_are_the_containing_pixel(tmp_path):
    r = raster(tmp_path, "a.tif", (np.arange(16).reshape(4, 4) * 10), 10.0, 10.004)  # row 0 is the north edge
    lon = np.array([10.0005, 10.0015, 10.0039, 10.0001])
    lat = np.array([10.0035, 10.0035, 10.0001, 10.0001])
    assert water.class_codes([r], lon, lat).tolist() == [0, 10, 150, 120]
    assert water.class_codes([r], np.array([9.99, 10.01]), np.array([10.002, 10.002])).tolist() == [0, 0]
    assert water.class_codes([r], lon.reshape(2, 2), lat.reshape(2, 2)).shape == (2, 2)


def test_first_raster_wins(tmp_path):
    a = raster(tmp_path, "a.tif", np.full((4, 4), 80), 10.0, 10.004)
    b = raster(tmp_path, "b.tif", np.full((8, 8), 10), 10.002, 10.006)  # overlaps a's north-east quarter
    lon, lat = np.array([10.003, 10.007, 9.5]), np.array([10.003, 10.001, 9.5])
    assert water.class_codes([a, b], lon, lat).tolist() == [80, 10, 0]
    assert water.class_codes([b, a], lon, lat).tolist() == [10, 10, 0]


def test_any_water(tmp_path):
    codes = np.full((10, 10), 10)
    codes[0, 9] = water.WATER  # the north-east pixel: lon 10.009-10.010, lat 10.009-10.010
    r = raster(tmp_path, "a.tif", codes, 10.0, 10.01)
    assert water.any_water([r], (10.0, 10.0, 10.005, 10.005)) is False
    assert water.any_water([r], (10.0085, 10.0085, 10.02, 10.02)) is True
    assert water.any_water([r], (11.0, 11.0, 11.1, 11.1)) is False
    assert water.any_water([], (10.0, 10.0, 10.01, 10.01)) is False


def test_reach_is_the_larger_distance():
    assert water.Water((), 200.0, 100.0).reach_m == 200.0
    assert water.Water((), 0.0, 100.0).reach_m == 100.0


def test_class_codes_with_target_m_reads_an_overview(tmp_path):
    # 400 x 400 px at 0.0001 deg (~11 m): west half water (80), east half land (10); overviews 2, 4, 8
    codes = np.full((400, 400), 10, np.uint8)
    codes[:, :200] = 80
    write_geotiff(tmp_path / "wc.tif", codes, 10.0, 10.04, 0.0001, overviews=(2, 4, 8), resampling="nearest")
    r = SourceRaster(path=tmp_path / "wc.tif", datum="wgs84")
    lon, lat = np.array([10.005, 10.035]), np.array([10.02, 10.02])
    np.testing.assert_array_equal(water.class_codes((r,), lon, lat), [80, 10])
    np.testing.assert_array_equal(water.class_codes((r,), lon, lat, target_m=90.0), [80, 10])
    assert water.class_codes((r,), lon, lat, target_m=5.0).tolist() == [80, 10]  # finer than the data: full resolution
