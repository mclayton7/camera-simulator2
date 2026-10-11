"""WGS-84 geodetic <-> ECEF and local East-North-Up frames (REALISM R3 building tiles). Degrees and metres; heights
are ellipsoid heights."""

from __future__ import annotations

import numpy as np

A = 6378137.0
F = 1.0 / 298.257223563
E2 = F * (2.0 - F)
B = A * (1.0 - F)
EP2 = (A * A - B * B) / (B * B)


def geodetic_to_ecef(lon, lat, h) -> np.ndarray:
    lon, lat, h = (np.asarray(v, np.float64) for v in (lon, lat, h))
    lo, la = np.radians(lon), np.radians(lat)
    n = A / np.sqrt(1.0 - E2 * np.sin(la) ** 2)
    return np.stack(
        [(n + h) * np.cos(la) * np.cos(lo), (n + h) * np.cos(la) * np.sin(lo), (n * (1.0 - E2) + h) * np.sin(la)],
        axis=-1,
    )


def ecef_to_geodetic(xyz) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Bowring's method plus two Newton refinements (sub-mm anywhere near the surface)."""
    xyz = np.asarray(xyz, np.float64)
    x, y, z = xyz[..., 0], xyz[..., 1], xyz[..., 2]
    p = np.hypot(x, y)
    th = np.arctan2(z * A, p * B)
    la = np.arctan2(z + EP2 * B * np.sin(th) ** 3, p - E2 * A * np.cos(th) ** 3)
    for _ in range(2):
        n = A / np.sqrt(1.0 - E2 * np.sin(la) ** 2)
        h = np.where(np.abs(np.cos(la)) > 1e-9, p / np.cos(la) - n, np.abs(z) - B)
        la = np.arctan2(z, p * (1.0 - E2 * n / (n + h)))
    n = A / np.sqrt(1.0 - E2 * np.sin(la) ** 2)
    h = np.where(np.abs(np.cos(la)) > 1e-9, p / np.cos(la) - n, np.abs(z) - B)
    return np.degrees(np.arctan2(y, x)), np.degrees(la), h


def enu_rotation(lon0: float, lat0: float) -> np.ndarray:
    lo, la = np.radians(lon0), np.radians(lat0)
    return np.array(
        [
            [-np.sin(lo), np.cos(lo), 0.0],
            [-np.sin(la) * np.cos(lo), -np.sin(la) * np.sin(lo), np.cos(la)],
            [np.cos(la) * np.cos(lo), np.cos(la) * np.sin(lo), np.sin(la)],
        ]
    )


def enu_to_ecef_matrix(lon0: float, lat0: float, h0: float) -> np.ndarray:
    m = np.eye(4)
    m[:3, :3] = enu_rotation(lon0, lat0).T  # columns E, N, U
    m[:3, 3] = geodetic_to_ecef(lon0, lat0, h0)
    return m


def matrix_to_column_major(m: np.ndarray) -> list[float]:
    return [float(v) for v in np.asarray(m, np.float64).T.reshape(16)]


def column_major_to_matrix(v) -> np.ndarray:
    return np.asarray(v, np.float64).reshape(4, 4).T
