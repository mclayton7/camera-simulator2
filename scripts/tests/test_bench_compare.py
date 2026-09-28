"""compare.py renders a readable delta table and a sane SSIM."""

import numpy as np
from bench import compare


def test_table_has_every_phase_and_metric_with_deltas():
    base = {
        "phases": {"orbit": {"wall_ms_p99": 50.0, "gpu_ms_p50": 30.0, "hitches_66": 10}}
    }
    cur = {
        "phases": {"orbit": {"wall_ms_p99": 40.0, "gpu_ms_p50": 15.0, "hitches_66": 2}}
    }
    table = compare.render_table(base, cur)
    assert "| orbit |" in table
    assert "wall_ms_p99" in table and "50.0" in table and "40.0" in table
    assert "-20%" in table  # 50 -> 40
    assert "-50%" in table  # 30 -> 15


def test_phase_missing_from_one_side_is_shown_not_dropped():
    table = compare.render_table(
        {"phases": {"orbit": {"wall_ms_p99": 1.0}}}, {"phases": {}}
    )
    assert "orbit" in table and "—" in table


def test_ssim_identical_is_one_and_noise_is_lower():
    rng = np.random.default_rng(0)
    a = rng.integers(0, 255, (72, 128, 3), dtype=np.uint8)
    b = np.clip(a.astype(int) + rng.integers(-60, 60, a.shape), 0, 255).astype(np.uint8)
    assert compare.ssim(a, a) == 1.0
    assert 0.0 < compare.ssim(a, b) < 0.95
