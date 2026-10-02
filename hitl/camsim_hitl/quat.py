"""Hamilton quaternions, w first (HITL.md "Frames, altitudes and conversions").

``q_AB`` rotates frame B into frame A: a vector expressed in B, ``v_B``, is
``v_A = q_AB * v_B * q_AB^-1``. Frames: N = local North-East-Down, B = body
forward-right-down, C = camera (x along the boresight, y right, z down).

Pure Python on purpose: the host does a few hundred of these per second.
"""

from __future__ import annotations

import math
from typing import NamedTuple

AXES = {"x": 0, "y": 1, "z": 2}


class Quat(NamedTuple):
    w: float
    x: float
    y: float
    z: float

    # -- algebra ---------------------------------------------------------
    def __mul__(self, o: "Quat") -> "Quat":  # type: ignore[override]
        aw, ax, ay, az = self
        bw, bx, by, bz = o
        return Quat(
            aw * bw - ax * bx - ay * by - az * bz,
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
        )

    def conj(self) -> "Quat":
        return Quat(self.w, -self.x, -self.y, -self.z)

    def inverse(self) -> "Quat":
        n = self.norm2()
        return Quat(self.w / n, -self.x / n, -self.y / n, -self.z / n)

    def norm2(self) -> float:
        return self.w * self.w + self.x * self.x + self.y * self.y + self.z * self.z

    def normalized(self) -> "Quat":
        n = math.sqrt(self.norm2())
        if n == 0.0 or not math.isfinite(n):
            return IDENTITY
        return Quat(self.w / n, self.x / n, self.y / n, self.z / n)

    def canonical(self) -> "Quat":
        """Same rotation with w >= 0."""
        return self if self.w >= 0.0 else Quat(-self.w, -self.x, -self.y, -self.z)

    def rotate(self, v: tuple[float, float, float]) -> tuple[float, float, float]:
        """Rotate vector v (expressed in the 'from' frame) into the 'to' frame."""
        w, x, y, z = self
        vx, vy, vz = v
        # t = 2 * cross(q.xyz, v); v' = v + w t + cross(q.xyz, t)
        tx = 2.0 * (y * vz - z * vy)
        ty = 2.0 * (z * vx - x * vz)
        tz = 2.0 * (x * vy - y * vx)
        return (
            vx + w * tx + (y * tz - z * ty),
            vy + w * ty + (z * tx - x * tz),
            vz + w * tz + (x * ty - y * tx),
        )

    def is_finite(self) -> bool:
        return all(math.isfinite(c) for c in self)

    def as_list(self) -> list[float]:
        return [self.w, self.x, self.y, self.z]


IDENTITY = Quat(1.0, 0.0, 0.0, 0.0)


def qx(a: float) -> Quat:
    """Rotation by a radians about x."""
    return Quat(math.cos(a / 2), math.sin(a / 2), 0.0, 0.0)


def qy(a: float) -> Quat:
    return Quat(math.cos(a / 2), 0.0, math.sin(a / 2), 0.0)


def qz(a: float) -> Quat:
    return Quat(math.cos(a / 2), 0.0, 0.0, math.sin(a / 2))


_AXIS_Q = {"x": qx, "y": qy, "z": qz}


def from_euler_zyx(yaw: float, pitch: float, roll: float) -> Quat:
    """qz(yaw) * qy(pitch) * qx(roll): aerospace yaw -> pitch -> roll (radians)."""
    return qz(yaw) * qy(pitch) * qx(roll)


def to_euler_zyx(q: Quat) -> tuple[float, float, float]:
    """(yaw, pitch, roll) radians, the HITL.md formulas; pitch in [-pi/2, pi/2]."""
    w, x, y, z = q
    yaw = math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z))
    pitch = math.asin(max(-1.0, min(1.0, 2 * (w * y - z * x))))
    roll = math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y))
    return yaw, pitch, roll


def to_matrix(q: Quat) -> list[list[float]]:
    w, x, y, z = q
    return [
        [1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
        [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
        [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)],
    ]


def wrap_pi(a: float) -> float:
    """Wrap to [-pi, pi)."""
    return (a + math.pi) % (2 * math.pi) - math.pi


def wrap_deg180(a: float) -> float:
    """Wrap to [-180, 180)."""
    return (a + 180.0) % 360.0 - 180.0


def unwrap_near(a: float, ref: float) -> float:
    """a + 2*pi*k closest to ref; an exact half turn resolves to +pi (turn right)."""
    d = wrap_pi(a - ref)
    if d <= -math.pi + 1e-9:
        d = math.pi
    return ref + d


def _parity(order: str) -> int:
    i, j, k = (AXES[c] for c in order)
    return 1 if (i, j, k) in ((0, 1, 2), (1, 2, 0), (2, 0, 1)) else -1


def from_euler(order: str, a: float, b: float, c: float) -> Quat:
    """Intrinsic Tait-Bryan rotation q_i(a) * q_j(b) * q_k(c) for order 'ijk'."""
    i, j, k = order
    return _AXIS_Q[i](a) * _AXIS_Q[j](b) * _AXIS_Q[k](c)


SINGULAR_COS = 1e-6


def to_euler(
    order: str, q: Quat, prev: tuple[float, float, float] | None = None
) -> tuple[float, float, float]:
    """Decompose q = q_i(a) q_j(b) q_k(c) (order 'ijk', three distinct axes).

    The middle angle b is in [-pi/2, pi/2]. With ``prev`` the outer angles are
    unwrapped to stay continuous with it, and at the singularity (|b| = 90 deg,
    e.g. nadir for 'zyx') the outer angle a keeps its previous value and c takes
    up the rest of the rotation, so neither jumps across it.
    """
    if len(set(order)) != 3 or any(ch not in AXES for ch in order):
        raise ValueError(f"bad Euler order {order!r}")
    i, j, k = (AXES[ch] for ch in order)
    s = _parity(order)
    r = to_matrix(q)
    sb = max(-1.0, min(1.0, s * r[i][k]))
    b = math.asin(sb)
    cb = math.cos(b)
    if cb > 1e-3 or prev is None:
        a = math.atan2(-s * r[j][k], r[k][k])
        c = math.atan2(-s * r[i][j], r[i][i])
        if cb > 1e-3:
            if prev is not None:
                a = unwrap_near(a, prev[0])
                c = unwrap_near(c, prev[2])
            return a, b, c
    # Near the singularity: hold a, solve c from the remainder q_j(-b) q_i(-a) q.
    a = prev[0] if prev is not None else 0.0
    rem = _AXIS_Q[order[1]](-b) * _AXIS_Q[order[0]](-a) * q
    comp = (rem.x, rem.y, rem.z)[k]
    c = 2.0 * math.atan2(comp, rem.w)
    c = unwrap_near(c, prev[2]) if prev is not None else wrap_pi(c)
    return a, b, c


def slerp(q0: Quat, q1: Quat, t: float) -> Quat:
    d = q0.w * q1.w + q0.x * q1.x + q0.y * q1.y + q0.z * q1.z
    if d < 0.0:
        q1 = Quat(-q1.w, -q1.x, -q1.y, -q1.z)
        d = -d
    if d > 0.9995:
        return Quat(*(a + t * (b - a) for a, b in zip(q0, q1))).normalized()
    th = math.acos(d)
    s0 = math.sin((1 - t) * th) / math.sin(th)
    s1 = math.sin(t * th) / math.sin(th)
    return Quat(*(s0 * a + s1 * b for a, b in zip(q0, q1)))


def from_rotvec(v: tuple[float, float, float]) -> Quat:
    ang = math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2)
    if ang < 1e-12:
        return Quat(1.0, v[0] / 2, v[1] / 2, v[2] / 2).normalized()
    s = math.sin(ang / 2) / ang
    return Quat(math.cos(ang / 2), v[0] * s, v[1] * s, v[2] * s)


def to_rotvec(q: Quat) -> tuple[float, float, float]:
    q = q.canonical()
    n = math.sqrt(q.x * q.x + q.y * q.y + q.z * q.z)
    if n < 1e-12:
        return (2 * q.x, 2 * q.y, 2 * q.z)
    ang = 2 * math.atan2(n, q.w)
    return (q.x / n * ang, q.y / n * ang, q.z / n * ang)


def angle_between(q0: Quat, q1: Quat) -> float:
    """Rotation angle (rad) of q0^-1 q1."""
    v = to_rotvec(q0.conj() * q1)
    return math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2)
