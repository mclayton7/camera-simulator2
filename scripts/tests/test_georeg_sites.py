"""Unit tests for scripts/georeg/pick_sites.py (REALISM R6 marker sites)."""

import math
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "georeg"))
import pick_sites  # noqa: E402


def test_plane_fit_slope_and_relief():
    xs, ys = np.meshgrid(np.arange(-25, 26, 5.0), np.arange(-25, 26, 5.0))
    z = 0.25 * xs + 3.0  # 14.04 deg up to the east
    slope, aspect, relief = pick_sites.plane_fit(xs.ravel(), ys.ravel(), z.ravel())
    assert slope == pytest.approx(math.degrees(math.atan(0.25)), abs=1e-6)
    assert aspect == pytest.approx(270.0, abs=1e-6)  # the downhill direction: west
    assert relief == pytest.approx(0.0, abs=1e-9)


def test_relief_measures_the_misfit():
    xs, ys = np.meshgrid(np.arange(-25, 26, 5.0), np.arange(-25, 26, 5.0))
    z = np.where(xs > 0, 1.0, 0.0)  # a 1 m step
    _, _, relief = pick_sites.plane_fit(xs.ravel(), ys.ravel(), z.ravel())
    assert relief > 0.2  # RMS misfit of a 1 m step after the best plane: 0.25 m
