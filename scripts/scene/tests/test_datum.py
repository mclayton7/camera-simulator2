from pathlib import Path

import numpy as np
import pyproj
import pytest

from camsim_scene import datum, tiling
from camsim_scene.datum import DatumError, DatumTransform

GRIDS = {"us_noaa_g2018u0.tif": Path(__file__).parent / "fixtures" / "grids" / "us_noaa_g2018u0.tif"}


def g18() -> DatumTransform:
    section = datum.manifest_section(["nad83_2011_navd88_geoid18"])
    return DatumTransform(section["datums"]["nad83_2011_navd88_geoid18"], GRIDS)


def test_known_point_100m_navd88_is_65_283m_ellipsoidal():
    # UTM 11N 470000 E 3685000 N (NAD83(2011)) -> geographic, then the pinned vertical step
    lon, lat = pyproj.Transformer.from_crs("EPSG:6340", "EPSG:6318", always_xy=True).transform(470000.0, 3685000.0)
    off = g18().vertical_offset(np.array([lon]), np.array([lat]))
    assert 100.0 + off[0] == pytest.approx(65.283, abs=0.001)


def test_pendleton_horizontal_shift_is_about_1_35_m():
    lon, lat = np.array([-117.43]), np.array([33.355])
    slon, slat = g18().to_source_geographic(lon, lat)
    _, _, dist = pyproj.Geod(ellps="GRS80").inv(lon, lat, slon, slat)
    assert 1.25 < dist[0] < 1.45


def test_wgs84_is_identity():
    dt = DatumTransform(datum.manifest_section(["wgs84"])["datums"]["wgs84"], {})
    lon, lat = np.array([10.5, -117.0]), np.array([1.0, 33.0])
    a, b = dt.to_source_geographic(lon, lat)
    assert np.array_equal(a, lon) and np.array_equal(b, lat)
    assert np.all(dt.vertical_offset(lon, lat) == 0.0)


def test_missing_grid_fails():
    entry = datum.manifest_section(["nad83_2011_navd88_geoid18"])["datums"]["nad83_2011_navd88_geoid18"]
    with pytest.raises(DatumError, match="us_noaa_g2018u0.tif"):
        DatumTransform(entry, {})


def test_manifest_section_lists_pipelines_and_grids():
    s = datum.manifest_section(["nad83_2011", "nad83_2011_navd88_geoid18"])
    assert s["target"] == "EPSG:7912" and s["epoch"] == 2010.0
    assert set(s["grids"]) == {"us_noaa_g2018u0.tif"} and s["grids"]["us_noaa_g2018u0.tif"]["sha256"] is None
    assert "vgridshift +grids=us_noaa_g2018u0.tif" in s["datums"]["nad83_2011_navd88_geoid18"]["to_itrf_3d"]
    assert "t_epoch=2010" in s["datums"]["nad83_2011"]["from_itrf_h"]
    assert datum.geoid_datum("geoid 18") == "nad83_2011_navd88_geoid18" and datum.geoid_datum("GEOID12A") is None


@pytest.mark.parametrize("z, bound_mm", [(9, 10.0), (12, 1.0), (16, 0.1)])
def test_offset_lattice_error_bound(z, bound_mm):
    dt = g18()
    s = tiling.tile_size_deg(z)
    x, y = int((-117.43 + 180) // s), int((33.355 + 90) // s)
    w, so, e, n = tiling.tile_bounds(z, x, y)
    lon, lat = np.meshgrid(np.linspace(w, e, 257), np.linspace(n, so, 257))
    exact = dt.vertical_offset(*dt.to_source_geographic(lon, lat))
    approx = datum.offset_lattice(dt, z, lon, lat)
    assert np.abs(approx - exact).max() * 1000 <= bound_mm


def test_offset_lattice_agrees_exactly_on_a_shared_edge():
    dt, z = g18(), 12
    s = tiling.tile_size_deg(z)
    x, y = int((-117.43 + 180) // s), int((33.355 + 90) // s)
    edge_lon = np.full(257, tiling.tile_bounds(z, x, y)[2])
    edge_lat = np.linspace(*tiling.tile_bounds(z, x, y)[1::2], 257)
    left = datum.offset_lattice(
        dt, z, np.concatenate([edge_lon - s / 2, edge_lon]), np.concatenate([edge_lat, edge_lat])
    )[257:]
    right = datum.offset_lattice(
        dt, z, np.concatenate([edge_lon, edge_lon + s / 2]), np.concatenate([edge_lat, edge_lat])
    )[:257]
    assert np.array_equal(left, right)
