import numpy as np
import pytest
from fakes import FakeHttp, fixture_json
from rasters import write_geotiff

from camsim_scene.licences import LicenceError
from camsim_scene.sources import make_source
from camsim_scene.sources.base import Area, Asset
from camsim_scene.sources.naip_pc import SAS_TOKEN, STAC_SEARCH, PcSigner
from camsim_scene.sources.wc_s2 import URL, cell_name

AREA = Area((-117.41, 33.20, -117.35, 33.25))
TOKEN_URL = SAS_TOKEN.format(account="naipeuwest", container="naip")


def test_naip_discovery_filters_year_and_keeps_unsigned_urls():
    src = make_source(
        "naip_pc", options={"year": "2022"}, http=FakeHttp({STAC_SEARCH: lambda body: fixture_json("stac_naip.json")})
    )
    (a,) = src.discover(AREA)
    assert a.id == "ca_m_3311754_nw_11_060_20220530" and "?" not in a.url and a.metadata["epsg"] == 26911
    ras = src.open("x.tif", a)
    assert ras.bands == (1, 2, 3) and ras.nodata_rule == "all_zero" and ras.datum == "nad83_2011"


def test_naip_follows_post_next_links():
    pages = [
        {
            "features": fixture_json("stac_naip.json")["features"][:1],
            "links": [{"rel": "next", "href": STAC_SEARCH, "method": "POST", "body": {"token": "p2"}}],
        },
        {"features": [], "links": []},
    ]
    http = FakeHttp({STAC_SEARCH: lambda body: pages[1] if body.get("token") == "p2" else pages[0]})
    assert len(make_source("naip_pc", http=http).discover(AREA)) == 1


def test_pc_signer_refreshes_on_force():
    tokens = iter(
        [
            {"token": "sig=A", "msft:expiry": "2026-10-08T00:00:00Z"},
            {"token": "sig=B", "msft:expiry": "2026-10-08T01:00:00Z"},
        ]
    )
    http = FakeHttp({TOKEN_URL: lambda params: next(tokens)})
    signer = PcSigner(http, now=lambda: 1_791_000_000.0)  # 2026-10-03
    url = "https://naipeuwest.blob.core.windows.net/naip/v002/x.tif"
    assert signer(url) == url + "?sig=A" and signer(url) == url + "?sig=A"
    assert signer(url, force=True) == url + "?sig=B"
    assert len(http.calls) == 2


def test_pc_signer_refreshes_near_expiry():
    tokens = iter(
        [
            {"token": "sig=A", "msft:expiry": "2026-10-08T00:00:00Z"},
            {"token": "sig=B", "msft:expiry": "2026-10-09T00:00:00Z"},
        ]
    )
    clock = [1_791_000_000.0]
    signer = PcSigner(FakeHttp({TOKEN_URL: lambda params: next(tokens)}), now=lambda: clock[0])
    url = "https://naipeuwest.blob.core.windows.net/naip/a.tif"
    assert signer(url).endswith("sig=A")
    clock[0] = 1_791_417_600.0 - 60  # one minute before the first expiry
    assert signer(url).endswith("sig=B")


def test_wc_s2_cells_and_urls():
    assert cell_name(33, -118) == "N33W118" and cell_name(-5, 7) == "S05E007"
    assert URL.format(lat="N33", name="N33W118") == (
        "https://esa-worldcover-s2.s3.eu-central-1.amazonaws.com/rgbnir/2021/N33/ESA_WorldCover_10m_2021_v200_N33W118_S2RGBNIR.tif"
    )
    routes = {("HEAD", URL.format(lat=n[:3], name=n)): (200, 486_000_000) for n in ("N33W118", "N33W117", "N32W117")}
    assets = make_source("wc_s2", http=FakeHttp(routes)).discover(Area((-117.9, 32.5, -116.5, 33.5)))
    assert [a.id for a in assets] == ["N32W117", "N33W118", "N33W117"]  # N32W118 is a 404 (ocean)
    assert assets[1].metadata["bbox"] == [-118, 33, -117, 34]


def test_wc_s2_refuses_a_file_without_cc_by(tmp_path):
    src = make_source("wc_s2", http=FakeHttp({}))
    ok = write_geotiff(
        tmp_path / "ok.tif",
        np.ones((4, 64, 64), np.uint16),
        -118,
        34,
        1 / 12000,
        tags={"license": "CC-BY 4.0 - https://creativecommons.org/licenses/by/4.0/"},
    )
    assert src.prepare(ok, Asset("N33W118", "u", sha256="00" * 32), cache=None) is None
    bad = write_geotiff(
        tmp_path / "bad.tif", np.ones((4, 64, 64), np.uint16), -118, 34, 1 / 12000, tags={"license": "CC-BY-NC"}
    )
    with pytest.raises(LicenceError):
        src.prepare(bad, Asset("N33W118", "u", sha256="00" * 32), cache=None)
