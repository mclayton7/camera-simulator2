"""Gimbal setpoint -> airframe-relative camera attitude -> CIGI View Control angles.

Implements HITL.md "Gimbal setpoint to View Control angles":

    q_NB = qz(psi) * qy(theta) * qx(phi)
    yaw frame: YAW_IN_EARTH_FRAME (64) / YAW_IN_VEHICLE_FRAME (32) on PX4 v1.18+,
               else the legacy rule: EARTH if YAW_LOCK else VEHICLE
    horizon-referenced (ROLL_LOCK and PITCH_LOCK):
        q_NC = q_sp                (earth)      or  qz(psi) * q_sp (vehicle)
    body-referenced (no lock flags, vehicle yaw):
        q_NC = q_NB * q_sp
    q_BC_target = q_NB^-1 * q_NC

plus the per-axis lock handling the HITL.md sketch omits ("Mixed lock
flags"): with only some locks set, each body-relative yaw/pitch/roll angle is
taken from the horizon-referenced target when its axis is locked and from
q_sp itself (airframe-following) when it is not. Yaw counts as locked when it
is earth-framed or when roll or pitch is locked (PX4's vehicle-frame yaw is a
heading-relative yaw in the horizontal plane). With both or neither of roll and
pitch locked this reduces exactly to the formulas above.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

from .quat import IDENTITY, Quat, from_euler_zyx, qz, to_euler, wrap_deg180

# GIMBAL_DEVICE_FLAGS
RETRACT = 1
NEUTRAL = 2
ROLL_LOCK = 4
PITCH_LOCK = 8
YAW_LOCK = 16
YAW_IN_VEHICLE_FRAME = 32
YAW_IN_EARTH_FRAME = 64
ACCEPTS_YAW_IN_EARTH_FRAME = 128
RC_EXCLUSIVE = 256
RC_MIXED = 512

EARTH = "earth"
VEHICLE = "vehicle"


def yaw_frame(flags: int) -> str:
    """The yaw frame of a GIMBAL_DEVICE_SET_ATTITUDE setpoint."""
    if flags & YAW_IN_EARTH_FRAME:
        return EARTH  # PX4 v1.18+
    if flags & YAW_IN_VEHICLE_FRAME:
        return VEHICLE  # PX4 v1.18+
    return EARTH if flags & YAW_LOCK else VEHICLE  # legacy rule, PX4 <= v1.17


def q_nb_from_euler_deg(psi: float, theta: float, phi: float) -> Quat:
    """X-Plane true_psi/theta/phi (deg) -> q_NB."""
    return from_euler_zyx(math.radians(psi), math.radians(theta), math.radians(phi))


def _axis_locks(flags: int) -> tuple[bool, bool, bool]:
    """(roll, pitch, yaw) referenced to the horizon/earth rather than the airframe."""
    roll = bool(flags & ROLL_LOCK)
    pitch = bool(flags & PITCH_LOCK)
    yaw = roll or pitch or yaw_frame(flags) == EARTH
    return roll, pitch, yaw


def setpoint_to_q_bc(
    q_nb: Quat,
    psi_rad: float,
    q_sp: Quat,
    flags: int,
    prev_ypr: tuple[float, float, float] | None = None,
) -> Quat:
    """PX4's setpoint (finite q_sp) -> airframe-relative target q_BC."""
    frame = yaw_frame(flags)
    q_h = q_sp if frame == EARTH else qz(psi_rad) * q_sp  # horizon-referenced q_NC
    roll_l, pitch_l, yaw_l = _axis_locks(flags)
    if roll_l and pitch_l:
        return (q_nb.conj() * q_h).normalized()
    if not (roll_l or pitch_l or yaw_l):
        return q_sp.normalized()  # q_NB^-1 * (q_NB * q_sp)
    # Mixed: per-axis selection in the body-relative yaw-pitch-roll decomposition.
    yh, ph, rh = to_euler("zyx", (q_nb.conj() * q_h).normalized(), prev_ypr)
    yb, pb, rb = to_euler("zyx", q_sp.normalized(), prev_ypr)
    return from_euler_zyx(yh if yaw_l else yb, ph if pitch_l else pb, rh if roll_l else rb)


def q_bc_to_setpoint(q_nb: Quat, psi_rad: float, q_bc: Quat, flags: int) -> Quat:
    """Inverse of setpoint_to_q_bc: the q_sp that would hold the camera at q_bc.

    Seeds the rate-mode integrator from the camera's actual attitude."""
    frame = yaw_frame(flags)
    q_nc = q_nb * q_bc
    q_h = q_nc if frame == EARTH else qz(-psi_rad) * q_nc
    roll_l, pitch_l, yaw_l = _axis_locks(flags)
    if roll_l and pitch_l:
        return q_h.normalized()
    if not (roll_l or pitch_l or yaw_l):
        return q_bc.normalized()
    yh, ph, rh = to_euler("zyx", q_h.normalized())
    yb, pb, rb = to_euler("zyx", q_bc.normalized())
    return from_euler_zyx(yh if yaw_l else yb, ph if pitch_l else pb, rh if roll_l else rb)


def status_quaternion(q_nb: Quat, psi_rad: float, q_bc: Quat, earth: bool) -> Quat:
    """GIMBAL_DEVICE_ATTITUDE_STATUS q (HITL.md): earth frame, or heading-relative."""
    q_nc = q_nb * q_bc
    return (q_nc if earth else qz(psi_rad).conj() * q_nc).normalized().canonical()


@dataclass
class ViewAngles:
    """CIGI View Control angles, degrees: yaw right +, pitch up +, roll right +."""

    yaw: float
    pitch: float
    roll: float


class ViewAngleConverter:
    """q_BC -> View Control yaw/pitch/roll with continuity across nadir.

    The yaw -> pitch -> roll decomposition is singular at pitch = -90 deg; there
    the previous yaw is kept and roll takes up the rest of the rotation, so
    neither jumps. Yaw is sent within +-180 deg (HITL.md "Yaw range")."""

    def __init__(self) -> None:
        self._prev: tuple[float, float, float] | None = None

    def convert(self, q_bc: Quat) -> ViewAngles:
        y, p, r = to_euler("zyx", q_bc.normalized(), self._prev)
        self._prev = (y, p, r)
        return ViewAngles(
            wrap_deg180(math.degrees(y)), math.degrees(p), wrap_deg180(math.degrees(r))
        )


def boresight_ned(q_nc: Quat) -> tuple[float, float, float]:
    """Unit boresight (camera +x) in NED."""
    return q_nc.rotate((1.0, 0.0, 0.0))


def boresight_above_horizon(q_nc: Quat, margin_deg: float = 0.0) -> bool:
    """True when the boresight does not point below the local horizon.

    The image centre then never meets the ground (CamSim keeps reporting its
    last ground hit, HITL.md "Camera component")."""
    d = boresight_ned(q_nc)[2]  # + is down
    return d <= math.sin(math.radians(margin_deg))


NEUTRAL_Q = IDENTITY
