"""Unit tests for the footprints gate (REALISM R3b) in scripts/vegetation_check.py."""

import math

import numpy as np
import pytest
import vegetation_check as vc

LAT0, LON0 = 33.3, -117.3
KX = 111_320.0 * math.cos(math.radians(LAT0))


def _pt(north_m, east_m):
    return (LAT0 + north_m / 111_320.0, LON0 + east_m / KX)


class _Mask:
    """A building occupying 0..20 m east and 0..20 m north of (LAT0, LON0)."""

    def masked(self, lon, lat):
        e = (np.asarray(lon) - LON0) * KX
        n = (np.asarray(lat) - LAT0) * 111_320.0
        return (e >= 0) & (e < 20) & (n >= 0) & (n < 20)


def test_probe_hits_count_trunks_and_axis_probes():
    trees = [_pt(10, 10), _pt(10, -1.5), _pt(10, -5.0), _pt(-1.0, -1.0)]  # inside, 1.5 m west, 5 m west, diagonal
    r = vc.footprint_gate(trees, _Mask(), 2.0)
    assert r["trees"] == 4 and r["probe_hits"] == 2 and not r["pass"]
    assert vc.footprint_gate([_pt(10, -5.0), _pt(-1.0, -1.0)], _Mask(), 2.0)["probe_hits"] == 0


def test_polygon_counts_deep_trunks_and_reports_the_buffer():
    shapely = pytest.importorskip("shapely")
    sq = shapely.box(LON0, LAT0, LON0 + 20 / KX, LAT0 + 20 / 111_320.0)
    trees = [_pt(10, 10), _pt(10, 0.3), _pt(-1.0, -1.0), _pt(10, -5.0)]  # deep inside, at the wall, corner 1.4 m, 5 m
    r = vc.footprint_gate(trees, _Mask(), 2.0, [sq])
    assert r["trunks_inside_footprints"] == 1  # the wall tree is within the mask's half-cell diagonal of the edge
    assert r["within_clearance"] == 3  # info: inside, at the wall, the corner tree (the axis probes miss corners)


def _square():
    shapely = pytest.importorskip("shapely")
    return shapely.box(LON0, LAT0, LON0 + 20 / KX, LAT0 + 20 / 111_320.0)


def test_clean_trees_pass_with_polygons():
    r = vc.footprint_gate([_pt(10, -5.0), _pt(-1.0, -1.0)], _Mask(), 2.0, [_square()])
    assert r["pass"] and r["mask_cells_in_bbox"] > 0 and r["footprints_in_bbox"] == 1


@pytest.mark.parametrize("polygons", [None, []])
def test_missing_or_empty_polygons_fail_closed(polygons):
    r = vc.footprint_gate([_pt(10, -5.0), _pt(-1.0, -1.0)], _Mask(), 2.0, polygons)
    assert r["probe_hits"] == 0 and not r["pass"] and any("polygons" in x for x in r["reasons"])


def test_empty_mask_fails_closed():
    class Empty:
        def masked(self, lon, lat):
            return np.zeros(np.shape(lon), bool)

    r = vc.footprint_gate([_pt(10, -5.0), _pt(-1.0, -1.0)], Empty(), 2.0, [_square()])
    assert r["probe_hits"] == 0 and r["mask_cells_in_bbox"] == 0 and not r["pass"]


def test_no_footprint_near_the_trees_fails_closed():
    far = [_pt(5000, 5000), _pt(5010, 5000)]  # nowhere near the building
    r = vc.footprint_gate(far, _Mask(), 2.0, [_square()])
    assert r["probe_hits"] == 0 and r["mask_cells_in_bbox"] == 0 and r["footprints_in_bbox"] == 0 and not r["pass"]


def test_probe_offsets_use_wgs84_radii():
    k_lat, k_lon = vc._deg_per_metre(np.array([0.0, 60.0]))
    # metres per degree of latitude: 110,574 at the equator, 111,412 at 60 N; of longitude at the equator 111,319.5
    assert abs(1 / k_lat[0] - 110_574) < 5
    assert abs(1 / k_lat[1] - 111_412) < 5
    assert abs(1 / k_lon[0] - 111_319.5) < 5
