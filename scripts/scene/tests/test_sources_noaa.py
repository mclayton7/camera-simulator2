"""NOAA topobathy adapters (R1 seabed): discovery by extent, nodata from the file, CRM's MSL shift."""

import numpy as np
import pytest
from fakes import FakeHttp
from rasters import write_geotiff

from camsim_scene.sources import make_source
from camsim_scene.sources.base import Area, Asset
from camsim_scene.sources.noaa_dem import CRM_URL, SD13_URL

PENDLETON_RING = (-118.709, 32.292, -116.151, 34.418)


def http():
    return FakeHttp({("HEAD", SD13_URL): (200, 445596180), ("HEAD", CRM_URL): (200, 524449536)})


def test_discover_inside_extent():
    for sid, url, size in (("noaa_sd13", SD13_URL, 445596180), ("noaa_crm_socal", CRM_URL, 524449536)):
        a = make_source(sid, http=http()).discover(Area(PENDLETON_RING))
        assert [(x.url, x.size, x.role) for x in a] == [(url, size, "data")]
        assert len(a[0].metadata["bbox"]) == 4


def test_discover_outside_extent_finds_nothing():
    sf = (-122.6, 37.6, -122.3, 37.9)
    assert make_source("noaa_sd13", http=http()).discover(Area(sf)) == []
    assert make_source("noaa_crm_socal", http=FakeHttp({})).discover(Area((-80.0, 25.0, -79.0, 26.0))) == []


def test_open_takes_nodata_from_the_file_and_crm_adds_its_shift(tmp_path):
    write_geotiff(
        tmp_path / "d.tif", np.array([[-5.0, -9999.0]], np.float32), -117.5, 33.3, 0.01, nodata=-9999.0,
        overviews=(),
    )
    sd = make_source("noaa_sd13").open(tmp_path / "d.tif", Asset("sd", SD13_URL))
    assert sd.nodata == -9999.0 and sd.add_m == 0.0 and sd.datum == "nad83_2011_navd88_geoid18"
    crm = make_source("noaa_crm_socal").open(
        tmp_path / "d.tif", Asset("crm", CRM_URL, metadata={"msl_above_navd88_m": 0.774})
    )
    assert crm.add_m == 0.774 and crm.nodata == -9999.0


def test_crm_open_without_the_shift_is_an_error(tmp_path):
    write_geotiff(tmp_path / "d.tif", np.zeros((1, 1), np.float32), -117.5, 33.3, 0.01, overviews=())
    with pytest.raises(ValueError, match="msl_above_navd88_m"):
        make_source("noaa_crm_socal").open(tmp_path / "d.tif", Asset("crm", CRM_URL))


@pytest.mark.network
def test_live_heads_answer():
    from camsim_scene.net import Http

    for sid in ("noaa_sd13", "noaa_crm_socal"):
        a = make_source(sid, http=Http()).discover(Area(PENDLETON_RING))
        assert a and a[0].size > 100_000_000
