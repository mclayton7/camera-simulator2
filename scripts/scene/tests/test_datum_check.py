"""scripts/scene/tools/datum_check.py (REALISM R6 datum gates D1 / D2) and fetch_ngs.py parsers."""

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import datum_check as dc  # noqa: E402
import fetch_ngs  # noqa: E402

REPO = Path(__file__).resolve().parents[3]
PKG = REPO / ".cache" / "scene-packages" / "pendleton-r5veg"

NDE = [
    {"pid": "DX1365", "corsId": " ", "lat": "33.3434444444", "lon": "-117.5134833333", "ellipHeight": "15.446",
     "posDatum": "NAD 83(2011)", "epoch": "2010.00", "orthoHt": "50.026", "vertDatum": "NAVD 88",
     "vertSource": "ADJUSTED", "vertOrder": "1", "geoidHt": "-34.620", "geoidModel": "GEOID18"},
    {"pid": "DX1460", "corsId": " ", "lat": "33.41", "lon": "-117.61", "ellipHeight": " ", "posDatum": "NAD 83(1986)",
     "epoch": " ", "orthoHt": "5.754", "vertDatum": "NAVD 88", "vertSource": "ADJUSTED", "vertOrder": "1",
     "geoidHt": "-34.819", "geoidModel": "GEOID18"},
    {"pid": "DG9736", "corsId": "P474", "lat": "33.355", "lon": "-117.2486", "ellipHeight": "183.0",
     "posDatum": "NAD 83(2011)", "epoch": "2010.00", "orthoHt": " ", "vertDatum": " ", "vertSource": " ",
     "vertOrder": " ", "geoidHt": "-33.3", "geoidModel": "GEOID18"},
]

CORS = """                              ***ITRF2014***
                FALLBROOK_CS2004 (P474),  CALIFORNIA
|         Antenna Reference Point(ARP): FALLBROOK_CS2004 CORS ARP             |
| ITRF2014 POSITION (EPOCH 2010.0)                                            |
| Computed in Jun 2019 using data through gpswk 1933.                         |
|     X =  -2441765.430 m     latitude    =  33 21 18.69364 N                 |
|     Y =  -4741246.409 m     longitude   = 117 14 55.29176 W                 |
|     Z =   3487030.733 m     ellipsoid height =  182.921   m                 |
| ITRF2014 VELOCITY                                                           |
|     VX =  -0.0294 m/yr      northward =   0.0164 m/yr                       |
| NAD_83 (2011) POSITION (EPOCH 2010.0)                                       |
|     X =  -2441764.669 m     latitude    =  33 21 18.68095 N                 |
|     Y =  -4741247.738 m     longitude   = 117 14 55.24209 W                 |
|     Z =   3487030.813 m     ellipsoid height =  183.661   m                 |
| L1 Phase Center of the current GPS antenna: FALLBROOK_CS2004 CORS L1PC      |
| ITRF2014 POSITION (EPOCH 2010.0)                                            |
|     X =  -2441765.461 m     latitude    =  33 21 18.69359 N                 |
|     Y =  -4741246.474 m     longitude   = 117 14 55.29170 W                 |
|     Z =   3487030.779 m     ellipsoid height =  183.007   m                 |
"""


def test_usable_marks_need_a_2011_ellipsoid_height_and_an_adjusted_levelled_height():
    marks = fetch_ngs.usable_marks(NDE)
    assert [m["pid"] for m in marks] == ["DX1365"]
    assert marks[0] == {"pid": "DX1365", "lat": 33.3434444444, "lon": -117.5134833333, "ellip_h": 15.446,
                        "navd88_h": 50.026, "geoid18_n": -34.62, "vert_order": "1"}


def test_cors_ids_come_from_the_marks():
    assert fetch_ngs.cors_ids(NDE) == ["P474"]


def test_parse_cors_takes_the_arp_block():
    c = fetch_ngs.parse_cors("P474", CORS)
    assert c["itrf2014"]["lat"] == pytest.approx(33 + 21 / 60 + 18.69364 / 3600)
    assert c["itrf2014"]["lon"] == pytest.approx(-(117 + 14 / 60 + 55.29176 / 3600))
    assert c["itrf2014"]["h"] == 182.921
    assert c["nad83_2011"]["h"] == 183.661
    assert c["epoch"] == 2010.0


def _grid():
    m = json.loads((PKG / "manifest.json").read_text())
    return m, dc.grid_path(m)


needs_pkg = pytest.mark.skipif(not (PKG / "manifest.json").exists(), reason="pendleton-r5veg not built here")


@needs_pkg
def test_d1_and_d2_pass_on_the_package_pipeline():
    m, grid = _grid()
    if not grid.exists():
        pytest.skip("GEOID18 grid not in the fetch cache")
    data = json.loads(dc.DATA.read_text())
    d1 = dc.check_geoid(m, grid, data["marks"])
    d2 = dc.check_frame(m, grid, data["cors"])
    assert d1["pass"], d1
    assert d2["pass"], d2


@needs_pkg
def test_d1_catches_a_flipped_geoid_sign():
    m, grid = _grid()
    if not grid.exists():
        pytest.skip("GEOID18 grid not in the fetch cache")
    bad = json.loads(json.dumps(m))
    e = bad["datum"]["datums"]["nad83_2011_navd88_geoid18"]
    e["to_itrf_3d"] = e["to_itrf_3d"].replace("+multiplier=1", "+multiplier=-1")
    d1 = dc.check_geoid(bad, grid, json.loads(dc.DATA.read_text())["marks"][:3])
    assert not d1["pass"]
