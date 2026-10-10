import numpy as np
import pytest
from fake_sources import coast_scene, fake_context, naip_s2_scene

from camsim_scene import config
from camsim_scene.context import WorkerState, class_rasters
from camsim_scene.ndvi_fit import NdviFit, fit_ndvi, linear_fit

S = {**config.NDVI, "reference": "naip_pc", "target": "wc_s2", "fit_step_m": 200.0, "min_samples": 20}


def test_json_round_trip_is_quantised():
    f = NdviFit.make("naip_pc", "wc_s2", 0.9000004, 0.0499996)
    assert (f.gain_e6, f.offset_e6) == (900000, 50000)
    g = NdviFit.from_json(f.to_json())
    assert g == f and g.to_json() == f.to_json() and b'"gain_e6": 900000' in f.to_json()


def test_another_format_is_refused():
    with pytest.raises(ValueError, match="format"):
        NdviFit.from_json(b'{"format": 0}')


def test_apply_clamps_and_keeps_nan():
    v = NdviFit.make("a", "b", 2.0, 0.5).apply(np.array([0.0, 0.5, -1.0, np.nan]))
    assert v[:3].tolist() == [0.5, 1.0, -1.0] and np.isnan(v[3])


def test_the_fit_is_the_same_for_shuffled_samples():
    rng = np.random.default_rng(1)
    x = rng.uniform(-0.2, 0.8, 5000)
    y = 0.9 * x + 0.05 + rng.normal(0, 0.02, 5000)
    p = rng.permutation(5000)
    a, b = (NdviFit.make("r", "t", *linear_fit(xx, yy)) for xx, yy in ((x, y), (x[p], y[p])))
    assert a.to_json() == b.to_json()


def test_fit_recovers_the_synthetic_gain_and_offset(tmp_path):
    st = WorkerState(fake_context(tmp_path, naip_s2_scene(tmp_path / "src", ndvi=True)))
    f = fit_ndvi(st.index["ndvi"], [], (10.0, 10.0, 10.2, 10.2), S, {"naip_pc/naip": "2022-05-30"})
    assert f.gain == pytest.approx(0.9, abs=0.02) and f.offset == pytest.approx(0.05, abs=0.01)
    r = f.report
    assert r["samples_fit"] >= 20 and r["samples_heldout"] > 0
    assert abs(r["heldout"]["bias_after"]) <= 0.005 and r["heldout"]["mae_after"] < r["heldout"]["mae_before"]
    assert list(r["bias_after_by_date"]) == ["2022-05-30"] and r["bias_after_by_date"]["2022-05-30"]["n"] > 0


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
