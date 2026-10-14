import logging

import pytest
from fakes import FakeHttp, fixture_json

from camsim_scene.net import Http, HttpError
from camsim_scene.sources import make_source
from camsim_scene.sources.base import Area, Layer
from camsim_scene.sources.dep3_1m import WESM_QUERY
from camsim_scene.sources.tnm import TNM_PRODUCTS

AREA = Area((-117.41, 33.20, -117.35, 33.25))


def tnm(name):
    return lambda params: (
        fixture_json(name) if params["offset"] == 0 else {"total": fixture_json(name)["total"], "items": []}
    )


def test_dep3_13_picks_newest_dated_copy():
    src = make_source("dep3_13", http=FakeHttp({TNM_PRODUCTS: tnm("tnm_13.json")}))
    assert src.layer == Layer.TERRAIN and src.max_zoom == 14
    (a,) = src.discover(AREA)
    assert a.id == "USGS_13_n34w118_20260915" and "/historical/n34w118/" in a.url and a.size == 351919765
    assert a.metadata["bbox"][0] == pytest.approx(-118.00055555589358) and src.version == "20260915"
    assert src.open(a.url, a).datum == "nad83_2011_navd88_geoid18"


def test_dep3_13_as_of_pins_an_older_copy():
    src = make_source("dep3_13", options={"as_of": "20251231"}, http=FakeHttp({TNM_PRODUCTS: tnm("tnm_13.json")}))
    assert src.discover(AREA)[0].id == "USGS_13_n34w118_20250826"


def test_dep3_1m_maps_geoids_orders_newest_first_and_skips_unknown(caplog):
    http = FakeHttp({TNM_PRODUCTS: tnm("tnm_1m.json"), WESM_QUERY: lambda params: fixture_json("wesm.json")})
    src = make_source("dep3_1m", http=http)
    with caplog.at_level(logging.WARNING):
        assets = src.discover(AREA)
    assert [a.id for a in assets] == ["USGS_1M_11_x46y368_CA_SanDiegoCo_D24"]
    a = assets[0]
    assert (
        a.group == "CA_SanDiegoCo_D24"
        and a.metadata["datum"] == "nad83_2011_navd88_geoid18"
        and a.metadata["utm_zone"] == 11
    )
    assert "San_Diego_CA_2014_LiDAR" in caplog.text and "GEOID12A" in caplog.text


def test_dep3_1m_geoid_override_includes_project_after_newer_one():
    http = FakeHttp({TNM_PRODUCTS: tnm("tnm_1m.json"), WESM_QUERY: lambda params: fixture_json("wesm.json")})
    src = make_source("dep3_1m", options={"geoid_overrides": {"San_Diego_CA_2014_LiDAR": "GEOID12B"}}, http=http)
    assets = src.discover(AREA)
    assert [a.group for a in assets] == ["CA_SanDiegoCo_D24", "San_Diego_CA_2014_LiDAR"]
    assert assets[1].metadata["datum"] == "nad83_2011_navd88_geoid12b" and assets[0].rank < assets[1].rank


@pytest.mark.network
def test_live_tnm_and_wesm_for_pendleton():
    http = Http()
    assert make_source("dep3_13", http=http).discover(AREA)
    one_m = make_source("dep3_1m", http=http).discover(AREA)
    assert any(a.metadata["datum"] == "nad83_2011_navd88_geoid18" for a in one_m)


def _wesm_down(params):
    raise HttpError("HTTP 400")


def test_dep3_1m_wesm_outage_with_full_overrides_proceeds(caplog):
    http = FakeHttp({TNM_PRODUCTS: tnm("tnm_1m.json"), WESM_QUERY: _wesm_down})
    ov = {"CA_SanDiegoCo_D24": "GEOID18", "San_Diego_CA_2014_LiDAR": "GEOID12B"}
    with caplog.at_level(logging.WARNING):
        assets = make_source("dep3_1m", options={"geoid_overrides": ov}, http=http).discover(AREA)
    assert [a.group for a in assets] == ["CA_SanDiegoCo_D24", "San_Diego_CA_2014_LiDAR"]
    assert "WESM unavailable" in caplog.text


def test_dep3_1m_wesm_outage_without_full_overrides_names_projects():
    http = FakeHttp({TNM_PRODUCTS: tnm("tnm_1m.json"), WESM_QUERY: _wesm_down})
    src = make_source("dep3_1m", options={"geoid_overrides": {"CA_SanDiegoCo_D24": "GEOID18"}}, http=http)
    with pytest.raises(HttpError, match="geoid_overrides for project.*San_Diego_CA_2014_LiDAR"):
        src.discover(AREA)


def test_dep3_13_as_of_before_every_copy_warns(caplog):
    src = make_source("dep3_13", options={"as_of": "20000101"}, http=FakeHttp({TNM_PRODUCTS: tnm("tnm_13.json")}))
    with caplog.at_level(logging.WARNING):
        assert src.discover(AREA) == []
    assert "n34w118" in caplog.text


TNM_ERROR = {
    "error": "Expecting value: line 1 column 1 (char 0)",
    "toastMessage": "Expecting value",
    "toastType": "warning",
}


def test_tnm_retries_an_error_body_instead_of_reading_it_as_no_products():
    # TNM answers HTTP 200 with an error body (2 of 6 calls, 2026-10-14): a socal-r5 replan lost all 12 dep3_1m assets
    from camsim_scene.sources.tnm import tnm_products

    answers = iter([TNM_ERROR, TNM_ERROR, {"total": 1, "items": [{"title": "a"}]}])
    http, slept = FakeHttp({TNM_PRODUCTS: lambda params: next(answers)}), []
    assert tnm_products(http, AREA.bounds, "d", sleep=slept.append) == [{"title": "a"}]
    assert len(slept) == 2


def test_tnm_persistent_error_body_raises():
    from camsim_scene.sources.tnm import tnm_products

    http = FakeHttp({TNM_PRODUCTS: lambda params: TNM_ERROR})
    with pytest.raises(HttpError, match="Expecting value"):
        tnm_products(http, AREA.bounds, "d", sleep=lambda s: None)
