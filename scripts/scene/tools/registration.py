"""Sub-pixel registration between two shots of the same pose (package vs Cesium World Terrain + Bing): phase
correlation on the luma of the central 512 x 512 window. Gate 5 passes below 1 px.

    uv run --project scripts/scene python scripts/scene/tools/registration.py out/package/nadir_2km.png out/cwt/nadir_2km.png
"""

from __future__ import annotations

import sys

import numpy as np
from PIL import Image


def luma_window(path: str, n: int = 512) -> np.ndarray:
    a = np.asarray(Image.open(path).convert("L"), np.float64)
    r0, c0 = (a.shape[0] - n) // 2, (a.shape[1] - n) // 2
    w = a[r0 : r0 + n, c0 : c0 + n]
    return (w - w.mean()) * np.outer(np.hanning(n), np.hanning(n))


def shift(a: np.ndarray, b: np.ndarray) -> tuple[float, float]:
    r = np.fft.fft2(a) * np.conj(np.fft.fft2(b))
    c = np.fft.ifft2(r / np.maximum(np.abs(r), 1e-12)).real
    i, j = np.unravel_index(np.argmax(c), c.shape)
    n = c.shape[0]

    def sub(m1: float, m0: float, p1: float) -> float:  # parabolic peak refinement
        d = m1 - 2 * m0 + p1
        return 0.0 if d == 0 else 0.5 * (m1 - p1) / d

    di = i + sub(c[i - 1, j], c[i, j], c[(i + 1) % n, j])
    dj = j + sub(c[i, j - 1], c[i, j], c[i, (j + 1) % n])
    return (di + n / 2) % n - n / 2, (dj + n / 2) % n - n / 2


if __name__ == "__main__":
    dy, dx = shift(luma_window(sys.argv[1]), luma_window(sys.argv[2]))
    mag = float(np.hypot(dx, dy))
    print(f"shift dx {dx:+.2f} px, dy {dy:+.2f} px, |d| {mag:.2f} px -> {'PASS' if mag < 1.0 else 'FAIL'}")
    sys.exit(0 if mag < 1.0 else 1)
