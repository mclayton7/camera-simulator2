"""REALISM R6 geo-registration geometry (scripts/georeg_check.py): a camera built from MISB ST 0601 KLV alone, as a
downstream user builds it, written from the ST 0601 definitions and independent of CamSim's C++.

Frames: WGS-84 ECEF; NED at the sensor. Platform attitude (Tags 5/6/7, or 90/91 for pitch/roll) then the sensor's
relative azimuth / elevation / roll (Tags 18/19/20), each z-y-x (yaw, pitch, roll), body x forward, y right, z down;
elevation is negative looking down. The camera looks along its x axis; image x is its y axis (right), image y its z axis
(down). Pinhole with fx from the horizontal FOV (Tag 16), fy from the vertical FOV (Tag 17), principal point at the
frame centre. Pixel coordinates are pixel-edge: pixel (i, j) covers [i, i+1) x [j, j+1) (docs/ground-truth.md)."""

from __future__ import annotations

import math
from collections.abc import Callable
from dataclasses import dataclass

import numpy as np

A = 6378137.0
F = 1 / 298.257223563
E2 = F * (2 - F)

# One least significant bit of each ST 0601 tag used here (degrees, metres)
LSB = {
    13: 180.0 / (2**32 - 2), 14: 360.0 / (2**32 - 2), 75: 19900.0 / 65535,
    5: 360.0 / 65535, 6: 40.0 / 65534, 7: 100.0 / 65534, 90: 180.0 / (2**32 - 2), 91: 180.0 / (2**32 - 2),
    16: 180.0 / 65535, 17: 180.0 / 65535,
    18: 360.0 / (2**32 - 1), 19: 360.0 / (2**32 - 2), 20: 360.0 / (2**32 - 1),
}


def ecef(lat: float, lon: float, h: float) -> np.ndarray:
    la, lo = math.radians(lat), math.radians(lon)
    n = A / math.sqrt(1 - E2 * math.sin(la) ** 2)
    return np.array([(n + h) * math.cos(la) * math.cos(lo), (n + h) * math.cos(la) * math.sin(lo),
                     (n * (1 - E2) + h) * math.sin(la)])


def geodetic(p: np.ndarray) -> tuple[float, float, float]:
    x, y, z = (float(v) for v in p)
    lon = math.atan2(y, x)
    r = math.hypot(x, y)
    lat = math.atan2(z, r * (1 - E2))
    for _ in range(10):
        n = A / math.sqrt(1 - E2 * math.sin(lat) ** 2)
        h = r / math.cos(lat) - n
        lat = math.atan2(z, r * (1 - E2 * n / (n + h)))
    n = A / math.sqrt(1 - E2 * math.sin(lat) ** 2)
    return math.degrees(lat), math.degrees(lon), r / math.cos(lat) - n


def ned_basis(lat: float, lon: float) -> np.ndarray:
    """Columns: north, east, down unit vectors in ECEF."""
    la, lo = math.radians(lat), math.radians(lon)
    north = np.array([-math.sin(la) * math.cos(lo), -math.sin(la) * math.sin(lo), math.cos(la)])
    east = np.array([-math.sin(lo), math.cos(lo), 0.0])
    down = -np.array([math.cos(la) * math.cos(lo), math.cos(la) * math.sin(lo), math.sin(la)])
    return np.column_stack([north, east, down])


def enu(lat0: float, lon0: float, h0: float, lat: float, lon: float, h: float) -> tuple[float, float, float]:
    d = ned_basis(lat0, lon0).T @ (ecef(lat, lon, h) - ecef(lat0, lon0, h0))
    return float(d[1]), float(d[0]), float(-d[2])


def offset_m(lat: float, lon: float, h: float, east: float, north: float, up: float = 0.0) -> tuple[float, float, float]:
    """The point `east`, `north` metres from (lat, lon) in its tangent plane, at height h + up."""
    p = ecef(lat, lon, h) + ned_basis(lat, lon) @ np.array([north, east, 0.0])
    la, lo, _ = geodetic(p)
    return la, lo, h + up


def zyx(yaw: float, pitch: float, roll: float) -> np.ndarray:
    """Body -> parent rotation, yaw about z, then pitch about y, then roll about x (degrees)."""
    y, p, r = (math.radians(v) for v in (yaw, pitch, roll))
    rz = np.array([[math.cos(y), -math.sin(y), 0], [math.sin(y), math.cos(y), 0], [0, 0, 1]])
    ry = np.array([[math.cos(p), 0, math.sin(p)], [0, 1, 0], [-math.sin(p), 0, math.cos(p)]])
    rx = np.array([[1, 0, 0], [0, math.cos(r), -math.sin(r)], [0, math.sin(r), math.cos(r)]])
    return rz @ ry @ rx


@dataclass
class Camera:
    lat: float
    lon: float
    h: float
    r_ned: np.ndarray  # camera -> NED at the sensor
    fx: float
    fy: float
    cx: float
    cy: float

    @classmethod
    def from_klv(cls, tags: dict, width: int, height: int) -> Camera:
        t = {int(k): float(v) for k, v in tags.items() if isinstance(v, (int, float))}
        pitch, roll = t.get(90, t[6]), t.get(91, t[7])
        r = zyx(t[5], pitch, roll) @ zyx(t[18], t[19], t.get(20, 0.0))
        fx = (width / 2) / math.tan(math.radians(t[16]) / 2)
        fy = (height / 2) / math.tan(math.radians(t[17]) / 2)
        return cls(t[13], t[14], t[75], r, fx, fy, width / 2, height / 2)

    def _cam(self, lat: float, lon: float, h: float) -> np.ndarray:
        d = ned_basis(self.lat, self.lon).T @ (ecef(lat, lon, h) - ecef(self.lat, self.lon, self.h))
        return self.r_ned.T @ d

    def project(self, lat: float, lon: float, h: float) -> tuple[float, float]:
        v = self._cam(lat, lon, h)
        if v[0] <= 0:
            raise ValueError("point is behind the camera")
        return self.cx + self.fx * v[1] / v[0], self.cy + self.fy * v[2] / v[0]

    def project_distorted(self, lat: float, lon: float, h: float, k1: float, k2: float) -> tuple[float, float]:
        """Pinhole, then r_d = r_u (1 + k1 r_u^2 + k2 r_u^4) on radii normalised by the focal length."""
        v = self._cam(lat, lon, h)
        xn, yn = v[1] / v[0], v[2] / v[0]
        r2 = xn * xn + yn * yn
        s = 1 + k1 * r2 + k2 * r2 * r2
        return self.cx + self.fx * xn * s, self.cy + self.fy * yn * s

    def ray(self, x: float, y: float) -> np.ndarray:
        """Unit ray through pixel (x, y) in ECEF."""
        v = np.array([1.0, (x - self.cx) / self.fx, (y - self.cy) / self.fy])
        d = ned_basis(self.lat, self.lon) @ (self.r_ned @ v)
        return d / np.linalg.norm(d)

    def unproject(self, x: float, y: float, h: float) -> tuple[float, float]:
        """Where the ray through pixel (x, y) first reaches ellipsoid height h: the nearer root on the ellipsoid
        with semi-axes a + h, b + h, refined by bisection on the true geodetic height."""
        o, d = ecef(self.lat, self.lon, self.h), self.ray(x, y)
        a, b = A + h, A * (1 - F) + h
        s = np.array([1 / a, 1 / a, 1 / b])
        oo, dd = o * s, d * s
        qa, qb, qc = dd @ dd, 2 * (oo @ dd), oo @ oo - 1
        disc = qb * qb - 4 * qa * qc
        if disc < 0:
            raise ValueError("ray never reaches that height")
        t0 = (-qb - math.sqrt(disc)) / (2 * qa)
        if t0 <= 0:
            raise ValueError("ray never reaches that height")
        lo, hi = max(0.0, t0 - 50.0), t0 + 50.0
        for _ in range(100):
            mid = 0.5 * (lo + hi)
            if geodetic(o + mid * d)[2] > h:
                lo = mid
            else:
                hi = mid
        la, lo_, _ = geodetic(o + 0.5 * (lo + hi) * d)
        return la, lo_

    def ground_pixel_size(self, lat: float, lon: float, h: float) -> tuple[float, float]:
        """Metres on the ground (at height h) per image pixel along image x and image y at the point."""
        x, y = self.project(lat, lon, h)

        def ground(px: float, py: float) -> np.ndarray:
            la, lo = self.unproject(px, py, h)
            return ecef(la, lo, h)

        return (float(np.linalg.norm(ground(x + 0.5, y) - ground(x - 0.5, y))),
                float(np.linalg.norm(ground(x, y + 0.5) - ground(x, y - 0.5))))


def corner_homography(tags: dict, width: int, height: int) -> Callable[[float, float], tuple[float, float]]:
    """lat/lon -> pixel through the four KLV corner points (Tags 82-89, ST 0601 order upper left, upper right, lower
    right, lower left) mapped to the frame's corners, in the tangent plane at the frame centre (Tags 23/24)."""
    lat0, lon0 = float(tags[23]), float(tags[24])
    src = [enu(lat0, lon0, 0.0, float(tags[k]), float(tags[k + 1]), 0.0)[:2] for k in (82, 84, 86, 88)]
    dst = [(0.0, 0.0), (float(width), 0.0), (float(width), float(height)), (0.0, float(height))]
    rows = []
    for (e, n), (x, y) in zip(src, dst):
        rows.append([e, n, 1, 0, 0, 0, -x * e, -x * n, -x])
        rows.append([0, 0, 0, e, n, 1, -y * e, -y * n, -y])
    _, _, vt = np.linalg.svd(np.array(rows, np.float64))
    hm = vt[-1].reshape(3, 3)

    def apply(lat: float, lon: float) -> tuple[float, float]:
        e, n, _ = enu(lat0, lon0, 0.0, lat, lon, 0.0)
        p = hm @ np.array([e, n, 1.0])
        return float(p[0] / p[2]), float(p[1] / p[2])

    return apply


def decode_rle(rle: dict) -> np.ndarray:
    from pycocotools import mask as cocomask

    return cocomask.decode({"size": rle["size"], "counts": rle["counts"].encode()
                            if isinstance(rle["counts"], str) else rle["counts"]})


def mask_centroid(rle: dict) -> tuple[float, float]:
    rows, cols = np.nonzero(decode_rle(rle))
    if len(rows) == 0:
        raise ValueError("empty mask")
    return float(cols.mean() + 0.5), float(rows.mean() + 0.5)


def mask_touches_edge(rle: dict) -> bool:
    m = decode_rle(rle)
    return bool(m[0].any() or m[-1].any() or m[:, 0].any() or m[:, -1].any())


def plate_attitude(heading: float, normal_az: float, normal_el: float) -> tuple[float, float, float]:
    """CIGI yaw, pitch, roll (degrees) of a plate heading `heading` whose up axis is the surface normal (azimuth from
    north, elevation above the horizontal, as CIGI's HAT/HOT extended response gives it)."""
    az, el = math.radians(normal_az), math.radians(normal_el)
    up = np.array([math.cos(el) * math.cos(az), math.cos(el) * math.sin(az), -math.sin(el)])  # NED, pointing up
    down = -up
    f0 = np.array([math.cos(math.radians(heading)), math.sin(math.radians(heading)), 0.0])
    f = f0 - (f0 @ down) * down
    f /= np.linalg.norm(f)
    right = np.cross(down, f)
    pitch = math.asin(max(-1.0, min(1.0, -f[2])))
    roll = math.asin(max(-1.0, min(1.0, right[2] / math.cos(pitch))))
    return math.degrees(math.atan2(f[1], f[0])) % 360.0, math.degrees(pitch), math.degrees(roll)


def plate_corners(lat: float, lon: float, h: float, half_m: float, top_m: float, heading: float, normal_az: float,
                  normal_el: float) -> list[tuple[float, float, float]]:
    """The four top corners of a plate whose origin (bottom-face centre) is at (lat, lon, h)."""
    r = zyx(*plate_attitude(heading, normal_az, normal_el))
    out = []
    for bx, by in ((half_m, -half_m), (half_m, half_m), (-half_m, half_m), (-half_m, -half_m)):
        n, e, d = r @ np.array([bx, by, -top_m])
        out.append(offset_m(lat, lon, h, east=float(e), north=float(n), up=float(-d)))
    return out


def projected_centroid(project: Callable[..., tuple[float, float]], corners) -> tuple[float, float]:
    """Area centroid of the quadrilateral the corners project to."""
    pts = [project(*c) for c in corners]
    a = cx = cy = 0.0
    for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
        c = x0 * y1 - x1 * y0
        a += c
        cx += (x0 + x1) * c
        cy += (y0 + y1) * c
    a *= 0.5
    return cx / (6 * a), cy / (6 * a)


def quantisation_budget(tags: dict, width: int, height: int, point) -> dict[int, float]:
    """Pixel shift of the projected point caused by one LSB of each tag the camera reads."""
    base = Camera.from_klv(tags, width, height).project(*point)
    out = {}
    for k in (13, 14, 75, 5, 6, 7, 90, 91, 18, 19, 20, 16, 17):
        if k not in tags or (k in (6, 7) and (k + 84) in tags):
            continue
        t = dict(tags)
        t[k] = float(t[k]) + LSB[k]
        x, y = Camera.from_klv(t, width, height).project(*point)
        out[k] = math.hypot(x - base[0], y - base[1])
    return out
