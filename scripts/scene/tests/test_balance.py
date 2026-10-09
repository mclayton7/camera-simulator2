import math

import numpy as np
import pytest

from camsim_scene import balance
from camsim_scene.balance import Balance, Grid

GRID = Grid(10.0, 10.0, 0.02, 0.02, 2, 1)  # two cells side by side


def make(offsets=None, xq=None, yq=None, grid=GRID):
    xq = xq or [np.array([0.0, 1000.0])] * 3
    yq = yq or [np.array([0.0, 100.0])] * 3
    off = np.zeros((3, grid.ny, grid.nx)) if offsets is None else offsets
    return Balance.make("naip_pc", "wc_s2", 200.0, xq, yq, grid, off)


def test_tone_interpolates_per_band():
    b = make(yq=[np.array([0.0, 100.0]), np.array([0.0, 50.0]), np.array([10.0, 20.0])])
    t = b.tone(np.full((3, 2), 500.0))
    assert t[:, 0].tolist() == [50.0, 25.0, 15.0]


def test_tone_clamps_outside_the_fitted_range():
    b = make()
    t = b.tone(np.array([[-50.0, 5000.0]] * 3))
    assert t[0].tolist() == [0.0, 100.0]


def test_offset_is_bilinear_between_cell_centres_and_clamped():
    off = np.zeros((3, 1, 2))
    off[:, 0, 1] = 10.0
    b = make(offsets=off)
    lat = np.array([10.01] * 4)
    lon = np.array([10.01, 10.03, 10.02, 10.5])  # centre 0, centre 1, midway, far east
    assert b.offset_at(lon, lat)[0].tolist() == pytest.approx([0.0, 10.0, 5.0, 10.0])


def test_apply_rounds_and_clips():
    off = np.full((3, 1, 2), 200.0)
    b = make(offsets=off)
    out = b.apply(np.full((3, 1), 1000.0), np.array([10.01]), np.array([10.01]))
    assert out.dtype == np.uint8 and out[:, 0].tolist() == [255, 255, 255]


def test_make_quantises_and_drops_tied_quantiles():
    b = make(xq=[np.array([1.004, 1.0049, 2.0])] * 3, yq=[np.array([1.04, 1.06, 2.0])] * 3)
    assert b.xq[0].tolist() == pytest.approx([1.0, 2.0])  # 1.004 and 1.0049 both round to 1.00: first kept
    assert b.yq[0].tolist() == pytest.approx([1.0, 2.0])  # 1.04 -> 1.0


def test_json_round_trip_is_exact():
    rng = np.random.default_rng(1)
    g = Grid.covering((-118.7, 32.3, -116.2, 34.4), 2.0, 33.35)
    off = rng.normal(0, 7, (3, g.ny, g.nx))
    xq = [np.sort(rng.uniform(0, 5000, 257)) for _ in range(3)]
    yq = [np.sort(rng.uniform(0, 255, 257)) for _ in range(3)]
    b = Balance.make("naip_pc", "wc_s2", 200.0, xq, yq, g, off)
    data = b.to_json()
    c = Balance.from_json(data)
    assert c.to_json() == data
    raw = rng.uniform(0, 6000, (3, 1000))
    lon, lat = rng.uniform(-118.7, -116.2, 1000), rng.uniform(32.3, 34.4, 1000)
    assert np.array_equal(b.apply(raw, lon, lat), c.apply(raw, lon, lat))
    assert b"report" not in data


def test_grid_covering_and_index():
    g = Grid.covering((10.0, 20.0, 10.1, 20.05), 2.0, 20.0)
    assert g.cell_lat == pytest.approx(2000.0 / 111320.0)
    assert g.cell_lon == pytest.approx(g.cell_lat / math.cos(math.radians(20.0)), abs=1e-9)
    assert g.nx == math.ceil(0.1 / g.cell_lon) and g.ny == math.ceil(0.05 / g.cell_lat)
    ix, iy = g.index(np.array([9.0, 10.0001, 11.0]), np.array([20.0001, 20.0001, 30.0]))
    assert ix.tolist() == [0, 0, g.nx - 1] and iy.tolist() == [0, 0, g.ny - 1]


def test_cell_medians_respect_the_minimum_count():
    resid = np.array([[1.0, 3.0, 2.0, 9.0]] * 3)
    cells = np.array([0, 0, 0, 1])
    m = balance.cell_medians(resid, cells, 3, min_n=2)
    assert m[0].tolist()[:1] == [2.0] and np.isnan(m[0, 1]) and np.isnan(m[0, 2])


def test_fill_decays_from_the_nearest_fitted_cell():
    v = np.full((3, 1, 5), np.nan)
    v[:, 0, 0] = 10.0
    out = balance.fill_offsets(v, cell_km=2.0, decay_km=10.0)
    assert out[0, 0].tolist() == pytest.approx([10.0 * math.exp(-2.0 * k / 10.0) for k in range(5)])


def test_median_filter_uses_fitted_cells_only():
    v = np.full((3, 3, 3), np.nan)
    v[:, 1, 1] = 100.0  # outlier
    v[:, 0, 1] = v[:, 1, 0] = v[:, 2, 1] = 0.0
    out = balance.fill_offsets(v, cell_km=2.0, decay_km=10.0)
    assert out[0, 1, 1] == 0.0  # median of {100, 0, 0, 0}: NaN neighbours ignored


def test_no_fitted_cells_gives_zero_offsets():
    out = balance.fill_offsets(np.full((3, 2, 2), np.nan), cell_km=2.0, decay_km=10.0)
    assert not out.any()


def test_median_size_widens_the_filter():
    v = np.full((3, 1, 5), 0.0)
    v[:, 0, 2] = 100.0
    v[:, 0, 0] = 50.0
    assert balance.fill_offsets(v, 2.0, 10.0)[0, 0, 2] == 0.0
    assert balance.fill_offsets(v, 2.0, 10.0, median_size=5)[0, 0, 2] == 0.0
    assert balance.fill_offsets(v, 2.0, 10.0, median_size=1)[0, 0, 2] == 100.0


def test_tone_curve_matches_a_known_mapping_within_a_dn():
    rng = np.random.default_rng(2)
    raw = rng.uniform(0, 3000, (3, 20000))
    truth = 255 * (raw / 3000) ** 0.8
    q = np.linspace(0, 1, 257)
    xq = [np.quantile(raw[b], q) for b in range(3)]
    yq = [np.quantile(truth[b], q) for b in range(3)]
    err = np.abs(Balance.make("a", "b", 200.0, xq, yq, GRID, np.zeros((3, 1, 2))).tone(raw) - truth)
    assert err.max() <= 2.0
    assert np.median(err) <= 1.0
