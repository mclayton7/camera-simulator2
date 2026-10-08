from types import SimpleNamespace

import pytest

from camsim_scene import licences
from camsim_scene.licences import DEFAULT_ALLOW, LicenceError, check_allowed


def test_default_allow_list_accepts_public_domain_and_cc_by():
    check_allowed({"dep3_13": "LicenseRef-PublicDomain-USGov", "worldcover": "CC-BY-4.0"}, DEFAULT_ALLOW)


def test_source_outside_the_allow_list_is_refused_with_the_flag_to_use():
    with pytest.raises(LicenceError, match=r"osm: licence 'ODbL-1.0'.*--allow ODbL-1.0"):
        check_allowed({"osm": "ODbL-1.0"}, DEFAULT_ALLOW)
    check_allowed({"osm": "ODbL-1.0"}, (*DEFAULT_ALLOW, "ODbL-1.0"))


def test_unknown_licence_id_is_refused():
    with pytest.raises(LicenceError, match="unknown licence"):
        check_allowed({"x": "beerware"}, ("beerware",))


def test_attribution_text_is_sorted_and_complete():
    src = lambda i, lic: SimpleNamespace(id=i, dataset=f"DS {i}", version="1", attribution=f"credit {i}", licence=lic)
    m = SimpleNamespace(name="pendleton", sources=[src("wc", "CC-BY-4.0"), src("ab", "LicenseRef-PublicDomain-USGov")])
    text = licences.attribution_text(m)
    assert text.index("DS ab") < text.index("DS wc")
    assert "credit wc" in text and "https://creativecommons.org/licenses/by/4.0/" in text
    assert text == licences.attribution_text(m)
