"""calibrate_colours.py: vegetation colour vs the imagery under it, exposure-corrected (REALISM R4 visual review)."""

import numpy as np
import pytest
from vegetation import calibrate_colours as cc


def _srgb(lin):
    lin = np.asarray(lin, float)
    return np.where(lin <= 0.0031308, lin * 12.92, 1.055 * lin ** (1 / 2.4) - 0.055) * 255


def test_ratio_is_imagery_over_render_with_the_exposure_divided_out():
    bg, under, veg = np.array([0.20, 0.18, 0.12]), np.array([0.10, 0.11, 0.06]), np.array([0.40, 0.60, 0.05])
    off = np.tile(_srgb(bg), (100, 100, 1))
    off[40:60, 40:60] = _srgb(under)
    gain = 1.25                                   # the on frame's auto-exposure is 25 % brighter
    on = np.tile(_srgb(bg * gain), (100, 100, 1))
    on[40:60, 40:60] = _srgb(veg * gain)
    r = cc.colour_ratio(on.round().astype(np.uint8), off.round().astype(np.uint8))
    assert r["n"] == 400
    assert r["gain"] == pytest.approx([gain] * 3, rel=0.02)
    assert r["ratio"] == pytest.approx((under / veg).tolist(), rel=0.03)


def test_too_few_vegetation_pixels_is_no_answer():
    a = np.full((50, 50, 3), 120, np.uint8)
    assert cc.colour_ratio(a, a)["ratio"] is None


def test_new_factor_scales_the_old_one_and_clamps():
    assert cc.new_factor([0.5, 0.5, 0.5], [0.4, 1.2, 3.0]) == pytest.approx([0.2, 0.6, 1.0])
