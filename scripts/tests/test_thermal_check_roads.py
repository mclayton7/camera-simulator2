"""Unit tests for gate r (road vs verge, REALISM R2) in scripts/thermal_check.py."""

import numpy as np
import pytest
import thermal_check as tc


def test_road_masks_geometry():
    road, verge = tc.road_masks((200, 300), gsd_m=0.5, road_w_m=14.0, verge_side=+1)
    cols_r = np.nonzero(road.any(axis=0))[0]
    cols_v = np.nonzero(verge.any(axis=0))[0]
    assert abs((cols_r.min() + cols_r.max()) / 2 - 149.5) <= 1  # centred
    assert len(cols_r) == round((14.0 - 2 * tc.ROAD_EDGE_INSET_M) / 0.5)
    assert cols_v.min() > cols_r.max()  # right of travel = image right when heading up
    assert not (road & verge).any()


def test_road_row_pass_fail():
    y = np.full((200, 300), 100.0)
    road, verge = tc.road_masks((200, 300), 0.5, 14.0, +1)
    y[road] = 110.0
    assert tc.road_row("mwir", "noon", y, (road, verge), 6.0)["pass"]
    assert not tc.road_row("mwir", "noon", y, (road, verge), 12.0)["pass"]


@pytest.mark.parametrize(
    "w, gsd, road_w",
    [(300, 0.5, 14.0), (301, 0.5, 14.0), (1280, 0.0827, 17.0), (1281, 0.0827, 17.0)],
)
def test_road_masks_column_count_and_centre(w, gsd, road_w):
    road, _ = tc.road_masks((100, w), gsd, road_w, +1)
    cols = np.nonzero(road.any(axis=0))[0]
    assert len(cols) == round((road_w - 2 * tc.ROAD_EDGE_INSET_M) / gsd)
    assert np.all(np.diff(cols) == 1)
    assert abs((cols.min() + cols.max()) / 2 - (w - 1) / 2) <= 0.5


def test_road_masks_verge_left_and_extent():
    gsd = 0.5
    road, verge = tc.road_masks((200, 300), gsd, 14.0, -1)
    cols_v = np.nonzero(verge.any(axis=0))[0]
    cols_r = np.nonzero(road.any(axis=0))[0]
    assert cols_v.max() < cols_r.min()  # left of travel = image left
    x = (cols_v - 149.5) * gsd  # metres from the road centre line
    assert np.all(-x >= 7.0 + tc.ROAD_VERGE_M[0] - 1e-9)
    assert np.all(-x <= 7.0 + tc.ROAD_VERGE_M[1] + 1e-9)


def test_road_masks_use_the_central_rows_only():
    road, verge = tc.road_masks((200, 300), 0.5, 14.0, +1)
    rows = np.nonzero(road.any(axis=1))[0]
    assert np.array_equal(rows, np.nonzero(tc.central_mask((200, 300)).any(axis=1))[0])
    assert np.array_equal(rows, np.nonzero(verge.any(axis=1))[0])


def test_road_row_fields():
    y = np.full((200, 300), 100.0)
    masks = tc.road_masks((200, 300), 0.5, 14.0, +1)
    y[masks[0]] = 103.0
    r = tc.road_row("lwir", "night", y, masks, tc.ROAD_NIGHT_DN)
    assert (r["check"], r["band"], r["time"], r["threshold"]) == (
        "r",
        "lwir",
        "night",
        tc.ROAD_NIGHT_DN,
    )
    assert r["value"] == pytest.approx(3.0)
    assert r["pass"]


def test_road_row_nan_fails():
    y = np.full((200, 300), 100.0)
    road, verge = tc.road_masks((200, 300), 0.5, 14.0, +1)
    assert not tc.road_row("mwir", "noon", y, (road, np.zeros_like(verge)), 6.0)["pass"]


def test_roads_run_label_maps_to_its_group_and_band():
    assert tc.run_group("mwir_roads") == "roads"
    assert tc.run_band("lwir_roads") == "lwir"


def test_expected_rows_roads():
    rows = tc.expected_rows(["mwir", "lwir"], {"roads"})
    assert sorted(rows) == sorted(
        [("r", b, t) for b in ("mwir", "lwir") for t in ("noon", "night")]
    )
    assert not any(r[0] == "r" for r in tc.expected_rows(["mwir"], {"bands"}))


def test_road_class_temps_from_the_builder_log():
    """Slots 3 (asphalt) and 8 (grassland) of the camsim.Thermal.Log line; noon / night told apart by sunLux."""
    tail = " terrain=0 water=1 Tair=291.0 K cloud=0.00 epsZ=0.786 KLum=97.0 KFast=1.00 EClamp=704 signalScale=0.5 gainEv=0.70"
    lines = [
        "Thermal: classes[K] 280.0 283.0 280.0 285.6 280.0 285.5 281.0 282.0 282.6"
        + tail
        + " sunLux=0 entities=0",
        "Thermal: classes[K] 300.0 288.5 303.0 303.5 299.1 297.9 296.7 298.8 298.9"
        + tail
        + " sunLux=94633 entities=0",
        "Thermal: classes[K] 300.0 288.5 303.0 303.6 299.1 297.9 296.7 298.8"
        + tail
        + " sunLux=94633 entities=0",
        "Thermal: some warning",
    ]
    t = tc.road_class_temps(lines)
    assert t["night"] == (285.6, 282.6)
    asphalt, grass = t["noon"]  # the last noon line: only 8 classes logged
    assert asphalt == 303.6 and np.isnan(grass)
