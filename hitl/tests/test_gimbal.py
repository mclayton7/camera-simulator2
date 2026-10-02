"""Gimbal plant and the Gimbal Protocol v2 device over a real localhost UDP link."""

import math
import socket
import time

import pytest

from camsim_hitl import frames
from camsim_hitl.config import GimbalConfig
from camsim_hitl.gimbal import GimbalDevice, GimbalPlant
from camsim_hitl.mavlink_link import Framer, MavlinkLink, UdpTransport, mavlink
from camsim_hitl.quat import angle_between, from_euler_zyx, to_euler_zyx
from camsim_hitl.truth import TruthState

D = math.radians


def cfg(**kw) -> GimbalConfig:
    c = GimbalConfig(enabled=True, link="test")
    for k, v in kw.items():
        setattr(c, k, v)
    return c


# -- plant ------------------------------------------------------------------------


def test_plant_rate_limit_and_convergence():
    p = GimbalPlant(cfg(servo_tau_s=0.05, max_rate_dps=[90, 60, 120], max_accel_dps2=[1e4, 1e4, 1e4]))
    p.set_q(from_euler_zyx(0, 0, 0))
    target, fl = p.limit_target(p.decompose(from_euler_zyx(D(90), 0, 0)))
    assert fl == 0
    dt = 0.004
    prev = 0.0
    for _ in range(int(0.5 / dt)):
        p.step(target, dt)
        yaw = to_euler_zyx(p.q_bc())[0]
        assert yaw - prev <= D(120) * dt + 1e-9  # yaw joint max rate
        prev = yaw
    assert math.degrees(prev) == pytest.approx(60, abs=1.5)  # 0.5 s at 120 deg/s
    for _ in range(int(2.0 / dt)):
        p.step(target, dt)
    assert math.degrees(to_euler_zyx(p.q_bc())[0]) == pytest.approx(90, abs=0.01)


def test_plant_limits_and_failure_flags():
    p = GimbalPlant(cfg(pitch_limits_deg=[-90, 20]))
    p.set_q(from_euler_zyx(0, 0, 0))
    target, fl = p.limit_target(p.decompose(from_euler_zyx(0, D(45), 0)))
    assert fl & 2  # AT_PITCH_LIMIT
    for _ in range(1000):
        p.step(target, 0.004)
    assert math.degrees(to_euler_zyx(p.q_bc())[1]) == pytest.approx(20, abs=1e-6)


def test_plant_zxy_reaches_past_nadir():
    # yaw-roll-pitch joints with pitch to -120: the camera can look backwards-down
    p = GimbalPlant(cfg(joint_order=["yaw", "roll", "pitch"], pitch_limits_deg=[-135, 30]))
    p.set_q(from_euler_zyx(0, 0, 0))
    q_t = from_euler_zyx(D(180), D(-60), D(180))  # == pitch -120 straight back
    target, fl = p.limit_target(p.decompose(q_t))
    assert fl == 0
    assert math.degrees(target[2]) == pytest.approx(-120, abs=1e-6)
    assert abs(target[0]) < 1e-6  # no yaw flip needed


def test_plant_servo_lag():
    p = GimbalPlant(cfg(servo_tau_s=0.1, max_rate_dps=[1e3] * 3, max_accel_dps2=[1e6] * 3))
    p.set_q(from_euler_zyx(0, 0, 0))
    target, _ = p.limit_target(p.decompose(from_euler_zyx(0, D(-10), 0)))
    for _ in range(25):  # one tau
        p.step(target, 0.004)
    frac = math.degrees(to_euler_zyx(p.q_bc())[1]) / -10.0
    assert frac == pytest.approx(1 - math.exp(-1), abs=0.03)


# -- device over MAVLink -------------------------------------------------------------


def level_truth(psi_deg=0.0):
    def fn(t_ns):
        return TruthState(
            t_ns=t_ns, latitude=37.0, longitude=-122.0, elevation=100.0,
            q_nb=frames.q_nb_from_euler_deg(psi_deg, 0, 0), psi_deg=psi_deg, theta_deg=0.0, phi_deg=0.0,
            rates=(0.0, 0.0, 0.0), vel_ned=(0.0, 0.0, 0.0), true_airspeed=0.0, indicated_airspeed_ms=0.0,
            groundspeed=0.0, mag_psi=0.0, y_agl=100.0, terrain_msl=None, paused=False, replay=False,
        )
    return fn


class Autopilot:
    """PX4 stand-in on the other end of the UDP link."""

    def __init__(self, port: int):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.05)
        self.dest = ("127.0.0.1", port)
        self.enc = mavlink.MAVLink(None, srcSystem=1, srcComponent=1)
        self.parser = mavlink.MAVLink(None)
        self.parser.robust_parsing = True
        self.framer = Framer()

    def send(self, msg):
        self.sock.sendto(msg.pack(self.enc), self.dest)
        self.enc.seq = (self.enc.seq + 1) % 256

    def recv(self, duration: float, types=None):
        out = []
        end = time.monotonic() + duration
        while time.monotonic() < end:
            try:
                data = self.sock.recv(65535)
            except (TimeoutError, socket.timeout):
                continue
            for _, frame in self.framer.feed(data):
                for m in self.parser.parse_buffer(frame) or []:
                    if types is None or m.get_type() in types:
                        out.append(m)
        return out

    def wait_for(self, mtype, timeout=2.0, pred=lambda m: True):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for m in self.recv(0.05, {mtype}):
                if pred(m):
                    return m
        return None

    def command(self, target_comp, command, *params):
        ps = list(params) + [0.0] * (7 - len(params))
        self.send(mavlink.MAVLink_command_long_message(1, target_comp, command, 0, *ps))

    def set_attitude(self, flags, q, rates=(math.nan, math.nan, math.nan)):
        self.send(mavlink.MAVLink_gimbal_device_set_attitude_message(1, 154, flags, list(q), *rates))


@pytest.fixture
def rig():
    link = MavlinkLink("test", UdpTransport("udpin", "127.0.0.1", 0))
    dev = GimbalDevice(cfg(setpoint_timeout_s=0.3, servo_tau_s=0.02, max_rate_dps=[360] * 3,
                           max_accel_dps2=[1e4] * 3, initial_deg=[0, 0, 0]), 1, truth_fn=level_truth(30.0))
    link.register(dev)
    dev.start()
    link.start()
    ap = Autopilot(link.transport.local_port)
    ap.send(mavlink.MAVLink_heartbeat_message(2, 12, 0, 0, 4, 3))  # lets udpin learn our address
    yield dev, ap, link
    link.stop()
    dev.stop()


def test_discovery_heartbeat_and_information(rig):
    dev, ap, link = rig
    hb = ap.wait_for("HEARTBEAT", pred=lambda m: m.get_srcComponent() == 154)
    assert hb is not None and hb.type == 26 and hb.autopilot == 8
    ap.command(0, 512, 283)  # PX4: REQUEST_MESSAGE to 0/0 before discovery
    got = ap.recv(0.5, {"COMMAND_ACK", "GIMBAL_DEVICE_INFORMATION"})
    ack = next(m for m in got if m.get_type() == "COMMAND_ACK")
    info = next((m for m in got if m.get_type() == "GIMBAL_DEVICE_INFORMATION"), None)
    assert got.index(ack) < got.index(info)  # ACK first
    assert ack is not None and ack.result == 0 and ack.target_system == 1 and ack.target_component == 1
    assert info is not None and info.get_srcComponent() == 154 and info.gimbal_device_id == 0
    assert info.pitch_min == pytest.approx(D(-120)) and info.pitch_max == pytest.approx(D(30))
    assert info.cap_flags & (32 | 128) and info.cap_flags & 4096
    # a broadcast request for something the gimbal doesn't serve gets no ACK ...
    ap.command(0, 512, 259)
    assert ap.wait_for("COMMAND_ACK", timeout=0.4) is None
    # ... a targeted one is UNSUPPORTED
    ap.command(154, 512, 259)
    assert ap.wait_for("COMMAND_ACK", pred=lambda m: m.command == 512).result == 3


def test_setpoint_status_and_handoff(rig):
    dev, ap, link = rig
    q_sp = from_euler_zyx(D(90), D(-45), 0)  # vehicle-frame yaw +90, horizon pitch -45
    for _ in range(10):
        ap.set_attitude(frames.ROLL_LOCK | frames.PITCH_LOCK, q_sp)
        time.sleep(0.05)
    st = ap.wait_for("GIMBAL_DEVICE_ATTITUDE_STATUS", pred=lambda m: abs(m.q[0] - q_sp.canonical().w) < 1e-3)
    assert st is not None
    assert st.target_system == 0 and st.target_component == 0  # broadcast so PX4 forwards it
    assert st.flags & frames.YAW_IN_VEHICLE_FRAME and not st.flags & frames.YAW_IN_EARTH_FRAME
    assert st.flags & frames.ROLL_LOCK and st.flags & frames.PITCH_LOCK
    assert st.delta_yaw == pytest.approx(D(30), abs=1e-5)
    q_bc = dev.sample_q_bc(time.monotonic_ns() + 33_000_000)
    assert angle_between(q_bc, q_sp) < D(0.1)  # level airframe: q_BC = q_sp


def test_earth_frame_setpoint_and_status_frame(rig):
    dev, ap, link = rig
    q_sp = from_euler_zyx(D(0), D(-30), 0)  # look north, earth frame; airframe heads 030
    for _ in range(10):
        ap.set_attitude(frames.ROLL_LOCK | frames.PITCH_LOCK | frames.YAW_LOCK, q_sp)
        time.sleep(0.05)
    q_bc = dev.sample_q_bc(time.monotonic_ns())
    assert math.degrees(to_euler_zyx(q_bc)[0]) == pytest.approx(-30, abs=0.2)
    ap.recv(0.4)  # drain the statuses sent while slewing
    st = ap.recv(0.3, {"GIMBAL_DEVICE_ATTITUDE_STATUS"})[-1]
    assert st.flags & frames.YAW_IN_EARTH_FRAME
    y, p, r = (math.degrees(a) for a in to_euler_zyx(type(q_sp)(*st.q)))
    assert (y, p, r) == pytest.approx((0, -30, 0), abs=0.3)  # earth-frame status


def test_set_message_interval_status_rate(rig):
    dev, ap, link = rig
    ap.command(154, 511, 285, 20000)  # 50 Hz
    assert ap.wait_for("COMMAND_ACK", pred=lambda m: m.command == 511).result == 0
    msgs = ap.recv(0.6, {"GIMBAL_DEVICE_ATTITUDE_STATUS"})
    assert len(msgs) >= 20
    ap.command(154, 511, 285, -1)
    ap.wait_for("COMMAND_ACK")
    ap.recv(0.1)
    assert len(ap.recv(0.4, {"GIMBAL_DEVICE_ATTITUDE_STATUS"})) == 0


def test_hil_actuator_controls_dropped_cheaply(rig):
    dev, ap, link = rig
    for _ in range(50):
        ap.send(mavlink.MAVLink_hil_actuator_controls_message(0, [0.0] * 16, 0, 0))
    ap.command(0, 512, 283)
    assert ap.wait_for("GIMBAL_DEVICE_INFORMATION") is not None
    assert link.dropped >= 50


def test_rate_mode_hold_and_link_loss(rig):
    dev, ap, link = rig
    lock = frames.ROLL_LOCK | frames.PITCH_LOCK
    nan4 = [math.nan] * 4
    # PX4 v1.17 idle: q and rates all NaN -> hold the current attitude
    ap.set_attitude(lock, nan4)
    time.sleep(0.2)
    y0 = to_euler_zyx(dev.current()[0])[0]
    time.sleep(0.2)
    assert to_euler_zyx(dev.current()[0])[0] == pytest.approx(y0, abs=1e-6)
    # rate mode: yaw right at 0.5 rad/s, resent at 20 Hz
    t_end = time.monotonic() + 0.6
    while time.monotonic() < t_end:
        ap.set_attitude(lock, nan4, (0.0, 0.0, 0.5))
        time.sleep(0.05)
    y1 = to_euler_zyx(dev.current()[0])[0]
    assert y1 - y0 == pytest.approx(0.3, abs=0.08)
    # silence: rates zeroed after setpoint_timeout_s (0.3 s here)
    time.sleep(0.5)
    y2 = to_euler_zyx(dev.current()[0])[0]
    time.sleep(0.4)
    y3 = to_euler_zyx(dev.current()[0])[0]
    assert y3 == pytest.approx(y2, abs=1e-3)


def test_neutral_and_retract(rig):
    dev, ap, link = rig
    for _ in range(6):
        ap.set_attitude(frames.NEUTRAL, [1.0, 0.0, 0.0, 0.0], (0.0, 0.0, 0.0))
        time.sleep(0.05)
    time.sleep(0.3)
    q, flags, _ = dev.current()
    assert angle_between(q, from_euler_zyx(0, 0, 0)) < D(0.2)
    assert flags & frames.NEUTRAL and flags & frames.YAW_IN_VEHICLE_FRAME


def test_prediction_leads_the_plant():
    dev = GimbalDevice(cfg(servo_tau_s=0.0, max_rate_dps=[60] * 3, max_accel_dps2=[1e5] * 3, initial_deg=[0, 0, 0]),
                       1, truth_fn=level_truth(0.0))
    t0 = time.monotonic_ns()
    dev._t_ns = t0
    with dev._lock:
        from camsim_hitl.gimbal import Setpoint
        dev.sp = Setpoint(flags=frames.ROLL_LOCK | frames.PITCH_LOCK, q=from_euler_zyx(D(90), 0, 0),
                          received_ns=t0, valid=True)
    dev.step_to(t0 + 100_000_000)  # 0.1 s at 60 deg/s = 6 deg
    now_yaw = math.degrees(to_euler_zyx(dev.current()[0])[0])
    ahead = math.degrees(to_euler_zyx(dev.sample_q_bc(t0 + 200_000_000))[0])
    past = math.degrees(to_euler_zyx(dev.sample_q_bc(t0 + 50_000_000))[0])
    assert now_yaw == pytest.approx(6, abs=0.3)
    assert ahead == pytest.approx(12, abs=0.3)
    assert past == pytest.approx(3, abs=0.3)
    # prediction did not move the real plant
    assert math.degrees(to_euler_zyx(dev.current()[0])[0]) == pytest.approx(now_yaw)
