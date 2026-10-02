import math
import random

import pytest

from camsim_hitl import frames
from camsim_hitl.frames import (
    PITCH_LOCK,
    ROLL_LOCK,
    YAW_IN_EARTH_FRAME,
    YAW_IN_VEHICLE_FRAME,
    YAW_LOCK,
    ViewAngleConverter,
    boresight_above_horizon,
    boresight_ned,
    q_nb_from_euler_deg,
    setpoint_to_q_bc,
    status_quaternion,
    yaw_frame,
)
from camsim_hitl.quat import (
    IDENTITY,
    Quat,
    angle_between,
    from_euler,
    from_euler_zyx,
    from_rotvec,
    qx,
    qy,
    qz,
    slerp,
    to_euler,
    to_euler_zyx,
    to_rotvec,
)

D = math.radians
HORIZON = ROLL_LOCK | PITCH_LOCK


def close_q(a: Quat, b: Quat, tol=1e-9) -> bool:
    return angle_between(a, b) < tol


def sp(yaw_deg, pitch_deg, roll_deg=0.0) -> Quat:
    return from_euler_zyx(D(yaw_deg), D(pitch_deg), D(roll_deg))


# -- quaternion basics -------------------------------------------------------


def test_axis_rotations_are_right_handed_ned():
    # yaw +90 turns the body x axis (north) to east
    v = qz(D(90)).rotate((1, 0, 0))
    assert v == pytest.approx((0, 1, 0), abs=1e-12)
    # pitch +90 points the nose up (-down)
    v = qy(D(90)).rotate((1, 0, 0))
    assert v == pytest.approx((0, 0, -1), abs=1e-12)
    # roll +90 (right wing down) moves body y (right) to down
    v = qx(D(90)).rotate((0, 1, 0))
    assert v == pytest.approx((0, 0, 1), abs=1e-12)


def test_euler_zyx_round_trip_random():
    rnd = random.Random(1)
    for _ in range(500):
        y, p, r = rnd.uniform(-3.1, 3.1), rnd.uniform(-1.5, 1.5), rnd.uniform(-3.1, 3.1)
        assert to_euler_zyx(from_euler_zyx(y, p, r)) == pytest.approx((y, p, r), abs=1e-9)


@pytest.mark.parametrize("order", ["zyx", "zxy", "xyz", "xzy", "yxz", "yzx"])
def test_generic_euler_round_trip(order):
    rnd = random.Random(order)
    for _ in range(300):
        a, b, c = rnd.uniform(-3.1, 3.1), rnd.uniform(-1.5, 1.5), rnd.uniform(-3.1, 3.1)
        q = from_euler(order, a, b, c)
        assert to_euler(order, q) == pytest.approx((a, b, c), abs=1e-9)
        # the alternative solution (a+pi, pi-b, c+pi) is the same rotation
        assert close_q(from_euler(order, a + math.pi, math.pi - b, c + math.pi), q, 1e-9)


def test_euler_singularity_keeps_outer_angle():
    q = from_euler_zyx(D(40), D(-90), D(10))
    y, p, r = to_euler("zyx", q, prev=(D(40), D(-89), D(10)))
    assert y == pytest.approx(D(40), abs=1e-6)
    assert p == pytest.approx(D(-90), abs=1e-6)
    assert close_q(from_euler_zyx(y, p, r), q, 1e-6)


def test_slerp_and_rotvec():
    a, b = IDENTITY, qz(D(90))
    assert close_q(slerp(a, b, 0.5), qz(D(45)))
    v = (0.1, -0.2, 0.3)
    assert to_rotvec(from_rotvec(v)) == pytest.approx(v, abs=1e-12)


# -- yaw-frame rule ----------------------------------------------------------


def test_yaw_frame_rule():
    assert yaw_frame(YAW_IN_EARTH_FRAME) == frames.EARTH
    assert yaw_frame(YAW_IN_VEHICLE_FRAME | YAW_LOCK) == frames.VEHICLE  # v1.18 flag wins
    assert yaw_frame(YAW_LOCK) == frames.EARTH  # legacy
    assert yaw_frame(HORIZON) == frames.VEHICLE  # legacy


# -- HITL.md sign case ---------------------------------------------------------


def test_sign_case_heading0_yaw90_pitch_minus45_looks_east_down():
    q_nb = q_nb_from_euler_deg(0, 0, 0)
    q_bc = setpoint_to_q_bc(q_nb, 0.0, sp(90, -45), HORIZON)
    va = ViewAngleConverter().convert(q_bc)
    assert (va.yaw, va.pitch, va.roll) == pytest.approx((90, -45, 0), abs=1e-9)
    d = boresight_ned(q_nb * q_bc)
    assert d == pytest.approx((0, math.sqrt(0.5), math.sqrt(0.5)), abs=1e-12)  # east and down


def test_horizon_lock_compensates_airframe_roll_and_pitch():
    q_nb = q_nb_from_euler_deg(30, 10, -20)
    q_bc = setpoint_to_q_bc(q_nb, D(30), sp(0, -30), HORIZON)  # vehicle-frame yaw 0
    q_nc = q_nb * q_bc
    y, p, r = to_euler_zyx(q_nc)
    assert (math.degrees(y), math.degrees(p), math.degrees(r)) == pytest.approx((30, -30, 0), abs=1e-9)


def test_earth_locked_yaw_holds_while_airframe_yaws():
    centres = []
    for psi in range(0, 360, 15):
        q_nb = q_nb_from_euler_deg(psi, 3, 5)
        q_bc = setpoint_to_q_bc(q_nb, D(psi), sp(45, -60), HORIZON | YAW_LOCK)  # legacy earth
        centres.append(boresight_ned(q_nb * q_bc))
        q_bc2 = setpoint_to_q_bc(q_nb, D(psi), sp(45, -60), HORIZON | YAW_IN_EARTH_FRAME)
        assert close_q(q_bc, q_bc2)
    for c in centres:
        assert c == pytest.approx(centres[0], abs=1e-9)
    y = math.degrees(math.atan2(centres[0][1], centres[0][0]))
    assert y == pytest.approx(45, abs=1e-9)


def test_vehicle_frame_yaw_follows_heading():
    for psi in (0, 90, 200):
        q_nb = q_nb_from_euler_deg(psi, 0, 0)
        q_bc = setpoint_to_q_bc(q_nb, D(psi), sp(20, -10), HORIZON)
        d = boresight_ned(q_nb * q_bc)
        assert math.degrees(math.atan2(d[1], d[0])) % 360 == pytest.approx((psi + 20) % 360, abs=1e-9)


def test_body_referenced_without_locks():
    q_nb = q_nb_from_euler_deg(10, 20, 30)
    q_sp = sp(5, -15, 0)
    assert close_q(setpoint_to_q_bc(q_nb, D(10), q_sp, YAW_IN_VEHICLE_FRAME), q_sp)
    assert close_q(setpoint_to_q_bc(q_nb, D(10), q_sp, 0), q_sp)


def test_pitch_lock_only_roll_follows_airframe():
    # Airframe rolled 20 deg right; PITCH_LOCK alone: body-relative roll stays at
    # the setpoint's (0, i.e. follows the airframe), pitch is horizon-referenced.
    q_nb = q_nb_from_euler_deg(0, 0, 20)
    q_bc = setpoint_to_q_bc(q_nb, 0.0, sp(0, -30), PITCH_LOCK)
    y, p, r = to_euler_zyx(q_bc)
    assert math.degrees(r) == pytest.approx(0, abs=1e-9)
    # with both locks the gimbal counter-rolls
    q_bc2 = setpoint_to_q_bc(q_nb, 0.0, sp(0, -30), HORIZON)
    q_nc2 = q_nb * q_bc2
    assert math.degrees(to_euler_zyx(q_nc2)[2]) == pytest.approx(0, abs=1e-9)
    # and with pitch locked, a nose-up airframe does not move the horizon pitch
    q_nb3 = q_nb_from_euler_deg(0, 15, 0)
    q_bc3 = setpoint_to_q_bc(q_nb3, 0.0, sp(0, -30), PITCH_LOCK)
    assert math.degrees(to_euler_zyx(q_nb3 * q_bc3)[1]) == pytest.approx(-30, abs=1e-9)


def test_q_bc_to_setpoint_inverts():
    q_nb = q_nb_from_euler_deg(70, 5, -8)
    for flags in (HORIZON, HORIZON | YAW_LOCK, 0, PITCH_LOCK):
        q_sp = sp(25, -40, 0)
        q_bc = setpoint_to_q_bc(q_nb, D(70), q_sp, flags)
        back = frames.q_bc_to_setpoint(q_nb, D(70), q_bc, flags)
        assert close_q(setpoint_to_q_bc(q_nb, D(70), back, flags), q_bc, 1e-9)


# -- nadir continuity ----------------------------------------------------------


def test_nadir_continuity_through_pitch_minus_90():
    conv = ViewAngleConverter()
    yaws, rolls = [], []
    for k in range(0, 61):
        pitch = -60 - k  # -60 .. -120 deg through nadir, yaw 30
        q = from_euler_zyx(D(30), D(max(pitch, -90)), 0.0) if pitch >= -90 else from_euler_zyx(
            D(30 + 180), D(-180 - pitch), D(180)
        )
        va = conv.convert(q)
        assert close_q(sp(va.yaw, va.pitch, va.roll), q, 1e-6)
        yaws.append(va.yaw)
        rolls.append(va.roll)
    # at nadir itself the yaw is held, so the sequence has no jump there
    i = 30
    assert abs(yaws[i] - yaws[i - 1]) < 1e-6
    assert all(-180 <= y < 180 for y in yaws)


def test_view_yaw_within_pm180():
    conv = ViewAngleConverter()
    va = conv.convert(from_euler_zyx(D(270), D(-10), 0))
    assert va.yaw == pytest.approx(-90)


# -- status and boresight ------------------------------------------------------


def test_status_quaternion_frames():
    q_nb = q_nb_from_euler_deg(100, 0, 0)
    q_bc = sp(10, -20)
    q_e = status_quaternion(q_nb, D(100), q_bc, earth=True)
    q_v = status_quaternion(q_nb, D(100), q_bc, earth=False)
    assert math.degrees(to_euler_zyx(q_e)[0]) == pytest.approx(110)
    assert math.degrees(to_euler_zyx(q_v)[0]) == pytest.approx(10)
    assert math.degrees(to_euler_zyx(q_v)[1]) == pytest.approx(-20)


def test_boresight_above_horizon():
    assert boresight_above_horizon(sp(0, 5))
    assert boresight_above_horizon(sp(0, 0))
    assert not boresight_above_horizon(sp(0, -1))
