import json

import numpy as np
import pytest
from fake_sources import coast_scene, fake_context, naip_ndvi_truth, naip_s2_scene, s2_ndvi_truth

from camsim_scene import config
from camsim_scene.balance import Grid
from camsim_scene.context import WorkerState, class_rasters
from camsim_scene.ndvi_fit import NdviFit, fit_ndvi, fit_samples, linear_fit

S = {
    **config.NDVI,
    "reference": "naip_pc",
    "target": "wc_s2",
    "fit_step_m": 200.0,
    "min_samples": 20,
    "min_cell_samples": 20,
}
GRID = Grid(10.0, 10.0, 0.1, 0.1, 2, 2)  # 2 x 2 cells of 0.1 degree, centres 10.05 / 10.15


def test_json_round_trip_is_integers_only():
    f = NdviFit.make("naip_pc", "wc_s2", 0.9000004, 0.0499996, GRID, [[0.0100004, -0.02], [0.0, 0.03]])
    assert (f.gain_e6, f.offset_e6) == (900000, 50000)
    assert f.cell_offsets_e6 == ((10000, -20000), (0, 30000))
    g = NdviFit.from_json(f.to_json())
    assert g == f and g.to_json() == f.to_json() and g.grid == f.grid
    d = json.loads(f.to_json())
    assert d["format"] == 2 and d["grid"] == {
        "west_e9": 10_000_000_000,
        "south_e9": 10_000_000_000,
        "cell_lon_e9": 100_000_000,
        "cell_lat_e9": 100_000_000,
        "nx": 2,
        "ny": 2,
    }
    assert d["cell_offsets_e6"] == [[10000, -20000], [0, 30000]]  # row 0 is the southernmost


def test_another_format_is_refused():
    with pytest.raises(ValueError, match="format"):
        NdviFit.from_json(b'{"format": 0}')
    old = b'{"format": 1, "gain_e6": 900000, "offset_e6": 50000, "reference": "naip_pc", "target": "wc_s2"}'
    with pytest.raises(ValueError, match="format 1"):  # Task 4's file: refit, never read as a zero grid
        NdviFit.from_json(old)


def test_apply_adds_the_cell_offset_clamps_and_keeps_nan():
    f = NdviFit.make("a", "b", 2.0, 0.5, GRID, [[0.0, 0.1], [0.0, 0.1]])
    lon, lat = np.array([10.05, 10.15, 10.10, 10.15, 10.05]), np.full(5, 10.05)
    v = f.apply(np.array([0.0, 0.0, 0.0, 0.5, np.nan]), lon, lat)
    assert v[:4].tolist() == pytest.approx([0.5, 0.6, 0.55, 1.0]) and np.isnan(v[4])  # bilinear between centres
    assert f.apply(np.array([-1.0]), np.array([9.0]), np.array([9.0])).tolist() == [-1.0]  # clamped (edge cell)
    assert f.apply_global(np.array([0.0, 0.5])).tolist() == [0.5, 1.0]


def test_the_fit_is_the_same_for_shuffled_samples():
    rng = np.random.default_rng(1)
    x = rng.uniform(-0.2, 0.8, 5000)
    lon, lat = rng.uniform(10.0, 10.2, 5000), rng.uniform(10.0, 10.2, 5000)
    y = 0.9 * x + 0.05 + 0.03 * (lon > 10.1) + rng.normal(0, 0.02, 5000)
    p = rng.permutation(5000)
    a = NdviFit.make("r", "t", *linear_fit(x, y), GRID)
    b = NdviFit.make("r", "t", *linear_fit(x[p], y[p]), GRID)
    assert a.to_json() == b.to_json()
    g = Grid.covering((10.0, 10.0, 10.2, 10.2), 2.0, 10.1)
    s = {**S, "min_cell_samples": 10}
    fa = fit_samples("r", "t", x, y, lon, lat, g, s)
    fb = fit_samples("r", "t", x[p], y[p], lon[p], lat[p], g, s)
    assert fa.to_json() == fb.to_json() and fa.report == fb.report and fa.report["cells"] > 0


def test_fit_recovers_the_synthetic_gain_and_offset(tmp_path):
    st = WorkerState(fake_context(tmp_path, naip_s2_scene(tmp_path / "src", ndvi=True)))
    f = fit_ndvi(st.index["ndvi"], [], (10.0, 10.0, 10.2, 10.2), S, {"naip_pc/naip": "2022-05-30"})
    assert f.gain == pytest.approx(0.9, abs=0.02) and f.offset == pytest.approx(0.05, abs=0.01)
    r = f.report
    assert r["samples_fit"] >= 20 and r["samples_heldout"] > 0
    assert abs(r["heldout"]["bias_after"]) <= 0.005 and r["heldout"]["mae_after"] < r["heldout"]["mae_before"]
    assert list(r["bias_after_by_date"]) == ["2022-05-30"] and r["bias_after_by_date"]["2022-05-30"]["n"] > 0
    assert r["cells"] + r["cells_filled"] == f.nx * f.ny and r["cells"] >= 0.9 * f.nx * f.ny  # a thin north row
    assert r["grid"] == {"nx": f.nx, "ny": f.ny, "cell_km": S["cell_km"]}
    assert -0.01 <= r["cell_offset_min"] <= r["cell_offset_max"] <= 0.01  # nothing regional to remove


def test_a_regional_offset_is_removed_by_the_grid_not_by_the_global_fit(tmp_path):
    def ramp(lon, lat):  # NAIP DN offset west -> east: NAIP NDVI -0.1 at 10.0 E to +0.1 at 10.2 E (offset / 200)
        return 20.0 * (np.asarray(lon) - 10.1) / 0.1

    st = WorkerState(fake_context(tmp_path, naip_s2_scene(tmp_path / "src", offset=ramp, ndvi=True)))
    f = fit_ndvi(st.index["ndvi"], [], (10.0, 10.0, 10.2, 10.2), S)
    lat = np.linspace(10.02, 10.18, 400)
    for lon0 in (10.03, 10.17):  # a west and an east strip
        lon = np.full(lat.shape, lon0)
        n, truth = naip_ndvi_truth(lon, lat) + ramp(lon, lat) / 200.0, s2_ndvi_truth(lon, lat)
        assert abs(np.median(f.apply_global(n) - truth)) > 0.04
        assert abs(np.median(f.apply(n, lon, lat) - truth)) <= 0.015
    h = f.report["heldout"]
    assert h["mae_after"] < h["mae_global"] and f.report["cell_offset_max"] - f.report["cell_offset_min"] > 0.1


def test_cells_without_samples_are_filled_from_their_neighbours(tmp_path):
    st = WorkerState(fake_context(tmp_path, naip_s2_scene(tmp_path / "src", ndvi=True)))
    f = fit_ndvi(st.index["ndvi"], [], (10.0, 10.0, 10.4, 10.2), S)  # east half: no NAIP, no samples
    assert f.report["cells"] > 0 and f.report["cells_filled"] > 0
    assert f.report["cells"] + f.report["cells_filled"] == f.nx * f.ny


def test_no_shared_samples_is_not_fitted(tmp_path):
    scene = naip_s2_scene(tmp_path / "src", ndvi=True, s2_box=(11.0, 11.0, 11.2, 11.2))
    st = WorkerState(fake_context(tmp_path, scene))
    assert fit_ndvi(st.index["ndvi"], [], (10.0, 10.0, 10.2, 10.2), S) is None


def test_water_is_left_out_of_the_fit(tmp_path):
    ctx = fake_context(tmp_path, coast_scene(tmp_path / "src", ndvi=True))
    st = WorkerState(ctx)
    classes, _ = class_rasters(st.manifest, ctx.asset_paths)
    f = fit_ndvi(st.index["ndvi"], classes, (10.06, 10.08, 10.14, 10.12), {**S, "fit_step_m": 50.0})
    assert f.gain == pytest.approx(0.9, abs=0.03) and f.offset == pytest.approx(0.05, abs=0.015)  # not the sea's -0.3
    assert list(f.report["bias_after_by_date"]) == ["unknown"]
