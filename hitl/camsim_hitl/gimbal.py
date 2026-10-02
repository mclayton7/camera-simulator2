"""Gimbal Protocol v2 gimbal device (HITL.md "What the gimbal device does").

PX4 is the gimbal manager; this is the device it drives with
GIMBAL_DEVICE_SET_ATTITUDE. It is stabilised with X-Plane truth (PX4 sends an
identity attitude in HITL), runs a joint-space plant (joint order, limits, rate,
acceleration, first-order servo lag) in its own thread at >= 200 Hz, reports
GIMBAL_DEVICE_ATTITUDE_STATUS, and hands the airframe-relative camera attitude
q_BC(t) to the CIGI host, sampled (predicted) at the render time t_r rather than
"latest".

It only needs a ``truth_fn(t_ns) -> TruthState | None`` and, for MAVLink, a
link; it can run on its own port or share one with the camera.
"""

from __future__ import annotations

import collections
import copy
import logging
import math
import threading
import time
from dataclasses import dataclass
from typing import Callable

from . import frames
from .config import GimbalConfig
from .mavlink_link import Component, encode_str, mavlink, version_u32
from .quat import (
    IDENTITY,
    Quat,
    from_euler,
    from_euler_zyx,
    from_rotvec,
    to_euler,
    to_rotvec,
    unwrap_near,
    wrap_pi,
)

log = logging.getLogger(__name__)

JOINT_AXIS = {"roll": "x", "pitch": "y", "yaw": "z"}
AXIS_INDEX = {"roll": 0, "pitch": 1, "yaw": 2}

MSG_GIMBAL_DEVICE_INFORMATION = 283
MSG_GIMBAL_DEVICE_SET_ATTITUDE = 284
MSG_GIMBAL_DEVICE_ATTITUDE_STATUS = 285

# GIMBAL_DEVICE_CAP_FLAGS
CAP_HAS_RETRACT = 1
CAP_HAS_NEUTRAL = 2
CAP_HAS_ROLL_AXIS = 4
CAP_HAS_ROLL_FOLLOW = 8
CAP_HAS_ROLL_LOCK = 16
CAP_HAS_PITCH_AXIS = 32
CAP_HAS_PITCH_FOLLOW = 64
CAP_HAS_PITCH_LOCK = 128
CAP_HAS_YAW_AXIS = 256
CAP_HAS_YAW_FOLLOW = 512
CAP_HAS_YAW_LOCK = 1024
CAP_SUPPORTS_INFINITE_YAW = 2048
CAP_SUPPORTS_YAW_IN_EARTH_FRAME = 4096

# GIMBAL_DEVICE_ERROR_FLAGS
AT_ROLL_LIMIT = 1
AT_PITCH_LIMIT = 2
AT_YAW_LIMIT = 4

MAV_TYPE_GIMBAL = 26
MAV_AUTOPILOT_INVALID = 8
MAV_STATE_ACTIVE = 4
MAV_CMD_REQUEST_MESSAGE = 512
MAV_CMD_SET_MESSAGE_INTERVAL = 511
MAV_RESULT_ACCEPTED = 0
MAV_RESULT_DENIED = 2
MAV_RESULT_UNSUPPORTED = 3

TruthFn = Callable[[int], object]


@dataclass
class Joint:
    name: str
    lo: float  # rad
    hi: float
    max_rate: float  # rad/s
    max_accel: float  # rad/s^2
    angle: float = 0.0
    rate: float = 0.0
    at_limit: bool = False

    @property
    def continuous(self) -> bool:
        return self.hi - self.lo >= 2 * math.pi - 1e-6


class GimbalPlant:
    """Joint-space servo model: limits, rate, acceleration, first-order lag tau."""

    def __init__(self, cfg: GimbalConfig):
        order = [j.lower() for j in cfg.joint_order]
        if sorted(order) != ["pitch", "roll", "yaw"]:
            raise ValueError(f"gimbal.joint_order must be a permutation of yaw/pitch/roll, got {order}")
        self.order = order
        self.axes = "".join(JOINT_AXIS[j] for j in order)
        lim = {
            "roll": cfg.roll_limits_deg,
            "pitch": cfg.pitch_limits_deg,
            "yaw": cfg.yaw_limits_deg,
        }
        self.joints = [
            Joint(
                name=j,
                lo=math.radians(lim[j][0]),
                hi=math.radians(lim[j][1]),
                max_rate=math.radians(cfg.max_rate_dps[AXIS_INDEX[j]]),
                max_accel=math.radians(cfg.max_accel_dps2[AXIS_INDEX[j]]),
            )
            for j in order
        ]
        self.tau = max(0.0, cfg.servo_tau_s)

    def set_q(self, q_bc: Quat) -> None:
        for j, a in zip(self.joints, self.decompose(q_bc)):
            j.angle = min(max(a, j.lo), j.hi) if not j.continuous else a
            j.rate = 0.0

    def q_bc(self) -> Quat:
        a, b, c = (j.angle for j in self.joints)
        return from_euler(self.axes, a, b, c)

    def decompose(self, q: Quat) -> tuple[float, float, float]:
        """Joint angles for q: of the two Euler solutions, the one inside the
        limits and nearest the current joints."""
        cur = tuple(j.angle for j in self.joints)
        a, b, c = to_euler(self.axes, q, cur)
        alt = (a + math.pi, wrap_pi(math.pi - b), c + math.pi)
        best, best_cost = None, math.inf
        for sol in ((a, b, c), alt):
            sol = tuple(self._fit(j, x) for j, x in zip(self.joints, sol))
            cost = 0.0
            for j, x in zip(self.joints, sol):
                if not j.continuous:
                    cost += 100.0 * (max(0.0, j.lo - x) + max(0.0, x - j.hi))
                cost += abs(x - j.angle)
            if cost < best_cost:
                best, best_cost = sol, cost
        assert best is not None
        return best

    @staticmethod
    def _fit(j: Joint, x: float) -> float:
        """The x + 2 pi k a joint should aim for: nearest the current angle for a
        continuous joint, else inside the limits (nearest the current angle)."""
        if j.continuous:
            return unwrap_near(x, j.angle)
        cands = [x + 2 * math.pi * k for k in (-2, -1, 0, 1, 2)]
        inside = [v for v in cands if j.lo - 1e-9 <= v <= j.hi + 1e-9]
        if inside:
            return min(inside, key=lambda v: abs(v - j.angle))
        return min(cands, key=lambda v: max(j.lo - v, v - j.hi))

    def limit_target(self, target: tuple[float, float, float]) -> tuple[list[float], int]:
        """Clamp a joint target; returns (target, failure flags)."""
        out, fl = [], 0
        for j, x in zip(self.joints, target):
            if j.continuous:
                x = unwrap_near(x, j.angle)
                j.at_limit = False
            else:
                xc = min(max(x, j.lo), j.hi)
                j.at_limit = abs(xc - x) > 1e-6
                if j.at_limit:
                    fl |= {"roll": AT_ROLL_LIMIT, "pitch": AT_PITCH_LIMIT, "yaw": AT_YAW_LIMIT}[j.name]
                x = xc
            out.append(x)
        return out, fl

    def step(self, target: list[float], dt: float) -> None:
        if dt <= 0.0:
            return
        for j, x in zip(self.joints, target):
            err = x - j.angle
            w_des = err / self.tau if self.tau > 0 else err / dt
            # Never ask for more speed than can be shed before the target.
            w_stop = math.sqrt(2.0 * j.max_accel * abs(err)) if j.max_accel > 0 else math.inf
            w_des = math.copysign(min(abs(w_des), j.max_rate, w_stop), err)
            dw = w_des - j.rate
            lim = j.max_accel * dt if j.max_accel > 0 else math.inf
            j.rate += max(-lim, min(lim, dw))
            j.angle += j.rate * dt
            if not j.continuous and (j.angle < j.lo or j.angle > j.hi):
                j.angle = min(max(j.angle, j.lo), j.hi)
                j.rate = 0.0
            elif j.continuous:
                # keep the angle bounded without disturbing the servo
                if j.angle > 4 * math.pi or j.angle < -4 * math.pi:
                    j.angle = wrap_pi(j.angle)

    def snapshot(self) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
        return tuple(j.angle for j in self.joints), tuple(j.rate for j in self.joints)


@dataclass
class Setpoint:
    flags: int = 0
    q: Quat | None = None  # None = rate mode (or hold)
    rates: tuple[float, float, float] = (0.0, 0.0, 0.0)  # x (roll), y (pitch), z (yaw) rad/s
    hold: bool = False  # q and rates all NaN: hold the current attitude in the locked frame
    received_ns: int = 0
    valid: bool = False


@dataclass
class _Ctl:
    """Controller state stepped with the plant (copied for prediction)."""

    rate_q: Quat | None = None  # integrated rate-mode setpoint, in the setpoint frame
    rate_flags: int = 0
    last_q_sp: Quat | None = None  # last angle setpoint (seeds hold/rate mode)
    last_flags: int = 0
    drift: tuple[float, float, float] = (0.0, 0.0, 0.0)
    prev_ypr: tuple[float, float, float] | None = None
    failure: int = 0
    flags_in_force: int = frames.YAW_IN_VEHICLE_FRAME


@dataclass
class _Hist:
    t_ns: int
    joints: tuple[float, float, float]


@dataclass
class _Level:
    """Stand-in truth when none is available: level, heading north."""

    q_nb: Quat = IDENTITY
    psi_rad: float = 0.0
    rates: tuple[float, float, float] = (0.0, 0.0, 0.0)
    theta_deg: float = 0.0
    phi_deg: float = 0.0


class GimbalDevice(Component):
    def __init__(self, cfg: GimbalConfig, sysid: int, truth_fn: TruthFn | None = None):
        super().__init__(sysid, cfg.compid)
        self.cfg = cfg
        self.truth_fn = truth_fn or (lambda t: None)
        self.plant = GimbalPlant(cfg)
        r, p, y = (math.radians(v) for v in cfg.initial_deg)
        self.plant.set_q(from_euler_zyx(y, p, r))
        self.sp = Setpoint()
        self.ctl = _Ctl()
        self._lock = threading.RLock()
        self._hist: collections.deque[_Hist] = collections.deque(maxlen=1024)
        self._t_ns = time.monotonic_ns()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="gimbal-plant", daemon=True)
        self.status_interval_s = 1.0 / cfg.status_rate_hz if cfg.status_rate_hz > 0 else 0.0
        self._next_status = 0.0
        self._next_hb = 0.0
        self._prev_status_q: tuple[float, Quat] | None = None
        self._warned_no_truth = False
        self._autopilot_att: _Level | None = None  # AUTOPILOT_STATE_FOR_GIMBAL_DEVICE (SITL tests only)
        self.setpoints_rx = 0
        self.info_sent = 0
        self.status_sent = 0

    # -- lifecycle ---------------------------------------------------------
    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread.is_alive():
            self._thread.join(timeout=2.0)

    # -- truth -------------------------------------------------------------
    def _truth(self, t_ns: int):
        if self.cfg.vehicle_attitude == "autopilot":
            # SITL/bench only: stabilise against PX4's own estimate. Under HIL
            # PX4 v1.16+ sends identity here, so the rig always uses X-Plane truth.
            if self._autopilot_att is not None:
                return self._autopilot_att
        st = self.truth_fn(t_ns)
        if st is None:
            if not self._warned_no_truth:
                self._warned_no_truth = True
                log.warning("gimbal: no truth yet; stabilising against a level, north-facing airframe")
            return _Level()
        return st

    # -- control law -------------------------------------------------------
    def _target(self, plant: GimbalPlant, ctl: _Ctl, sp: Setpoint, truth, t_ns: int, dt: float) -> Quat:
        """Airframe-relative target q_BC for this step (mutates ctl)."""
        q_nb, psi = truth.q_nb, truth.psi_rad
        cfg = self.cfg
        if not sp.valid:
            ctl.flags_in_force = frames.YAW_IN_VEHICLE_FRAME
            r, p, y = (math.radians(v) for v in cfg.initial_deg)
            return from_euler_zyx(y, p, r)
        flags = sp.flags
        frame = frames.yaw_frame(flags)
        ctl.flags_in_force = (flags & (frames.RETRACT | frames.NEUTRAL | frames.ROLL_LOCK | frames.PITCH_LOCK | frames.YAW_LOCK)) | (
            frames.YAW_IN_EARTH_FRAME if frame == frames.EARTH else frames.YAW_IN_VEHICLE_FRAME
        )
        if flags & frames.RETRACT:
            r, p, y = (math.radians(v) for v in cfg.retract_deg)
            return from_euler_zyx(y, p, r)
        if flags & frames.NEUTRAL:
            r, p, y = (math.radians(v) for v in cfg.neutral_deg)
            return from_euler_zyx(y, p, r)
        if sp.q is not None:
            ctl.rate_q = None
            ctl.last_q_sp, ctl.last_flags = sp.q, flags
            q_sp = sp.q
        else:
            # Rate mode: integrate the rates into a setpoint in the locked frame,
            # seeded from the last angle setpoint in the same frame (so a hold
            # mid-slew still finishes the slew), else from the actual attitude.
            if ctl.rate_q is None or ctl.rate_flags != flags:
                if ctl.last_q_sp is not None and ctl.last_flags == flags:
                    ctl.rate_q = ctl.last_q_sp
                else:
                    q0 = frames.q_bc_to_setpoint(q_nb, psi, plant.q_bc(), flags)
                    if flags & frames.ROLL_LOCK:  # a stabilised hold keeps the horizon level
                        y0, p0, _ = to_euler("zyx", q0)
                        q0 = from_euler_zyx(y0, p0, 0.0)
                    ctl.rate_q = q0
                ctl.rate_flags = flags
            # Hold (PX4 v1.17's idle setpoint: flags 12, q and rates all NaN) and
            # link loss (no setpoint for setpoint_timeout_s) both zero the rates.
            lost = (t_ns - sp.received_ns) > cfg.setpoint_timeout_s * 1e9
            wx, wy, wz = (0.0, 0.0, 0.0) if (lost or sp.hold) else sp.rates
            if dt > 0 and (wx or wy or wz):
                yaw, pitch, roll = to_euler("zyx", ctl.rate_q, ctl.prev_ypr)
                pitch = max(-math.pi / 2 + 1e-3, min(math.pi / 2 - 1e-3, pitch + wy * dt))
                ctl.rate_q = from_euler_zyx(yaw + wz * dt, pitch, roll + wx * dt)
            q_sp = ctl.rate_q
        q_bc = frames.setpoint_to_q_bc(q_nb, psi, q_sp, flags, ctl.prev_ypr)
        ctl.prev_ypr = to_euler("zyx", q_bc, ctl.prev_ypr)
        # Optional stabilisation error: a bounded gyro-bias drift (5 s leak).
        bias = cfg.gyro_bias_dps
        if any(bias) and flags & (frames.ROLL_LOCK | frames.PITCH_LOCK | frames.YAW_LOCK):
            k = min(1.0, dt / 5.0)
            ctl.drift = tuple(d + math.radians(b) * dt - d * k for d, b in zip(ctl.drift, bias))
            q_bc = q_bc * from_rotvec(ctl.drift)
        return q_bc

    def _advance(self, plant: GimbalPlant, ctl: _Ctl, sp: Setpoint, t_ns: int, dt: float) -> None:
        truth = self._truth(t_ns)
        q_t = self._target(plant, ctl, sp, truth, t_ns, dt)
        target, ctl.failure = plant.limit_target(plant.decompose(q_t))
        plant.step(target, dt)

    # -- plant thread ------------------------------------------------------
    def _run(self) -> None:
        period = 1.0 / max(200.0, self.cfg.plant_rate_hz)
        next_t = time.monotonic()
        while not self._stop.is_set():
            now_ns = time.monotonic_ns()
            self.step_to(now_ns)
            now = time.monotonic()
            if self.link is not None:
                if now >= self._next_hb:
                    self._next_hb = now + 1.0 / max(0.1, self.cfg.heartbeat_rate_hz)
                    self.send_heartbeat()
                if self.status_interval_s > 0 and now >= self._next_status:
                    self._next_status = now + self.status_interval_s
                    self.send_status()
            next_t += period
            delay = next_t - time.monotonic()
            if delay < -0.1:
                next_t = time.monotonic()
            elif delay > 0:
                self._stop.wait(delay)

    def step_to(self, t_ns: int) -> None:
        """Advance the plant to host time t_ns in <= 5 ms sub-steps."""
        with self._lock:
            max_dt = 1.0 / max(200.0, self.cfg.plant_rate_hz)
            while self._t_ns < t_ns:
                dt_ns = min(t_ns - self._t_ns, int(max_dt * 1e9))
                self._t_ns += dt_ns
                self._advance(self.plant, self.ctl, self.sp, self._t_ns, dt_ns / 1e9)
                self._hist.append(_Hist(self._t_ns, self.plant.snapshot()[0]))

    # -- hand-off to the CIGI host ------------------------------------------
    def sample_q_bc(self, t_ns: int) -> Quat:
        """Actual airframe-relative camera attitude at host time t_ns.

        Past times interpolate the plant history; future times (the usual case:
        t_r is about a frame ahead) run a copy of the plant forward with the
        same setpoint and the truth predicted to each sub-step."""
        with self._lock:
            if t_ns <= self._t_ns:
                h = self._hist
                if not h:
                    return self.plant.q_bc()
                if t_ns <= h[0].t_ns:
                    return from_euler(self.plant.axes, *h[0].joints)
                for i in range(len(h) - 1, 0, -1):
                    if h[i - 1].t_ns <= t_ns:
                        a, b = h[i - 1], h[i]
                        u = (t_ns - a.t_ns) / max(1, b.t_ns - a.t_ns)
                        j = tuple(x + (y - x) * u for x, y in zip(a.joints, b.joints))
                        return from_euler(self.plant.axes, *j)
                return from_euler(self.plant.axes, *h[-1].joints)
            plant = copy.deepcopy(self.plant)
            ctl = copy.deepcopy(self.ctl)
            sp = copy.copy(self.sp)
            t = self._t_ns
        max_dt_ns = int(1e9 / max(200.0, self.cfg.plant_rate_hz))
        while t < t_ns:
            dt_ns = min(t_ns - t, max_dt_ns)
            t += dt_ns
            self._advance(plant, ctl, sp, t, dt_ns / 1e9)
        return plant.q_bc()

    def current(self) -> tuple[Quat, int, int]:
        """(q_BC, flags in force, failure flags) now."""
        with self._lock:
            return self.plant.q_bc(), self.ctl.flags_in_force, self.ctl.failure

    # -- MAVLink -----------------------------------------------------------
    def cap_flags(self) -> int:
        if self.cfg.cap_flags:
            return self.cfg.cap_flags
        f = CAP_HAS_RETRACT | CAP_HAS_NEUTRAL | CAP_SUPPORTS_YAW_IN_EARTH_FRAME
        for j in self.plant.joints:
            if j.hi - j.lo <= 1e-6:
                continue
            f |= {
                "roll": CAP_HAS_ROLL_AXIS | CAP_HAS_ROLL_FOLLOW | CAP_HAS_ROLL_LOCK,
                "pitch": CAP_HAS_PITCH_AXIS | CAP_HAS_PITCH_FOLLOW | CAP_HAS_PITCH_LOCK,
                "yaw": CAP_HAS_YAW_AXIS | CAP_HAS_YAW_FOLLOW | CAP_HAS_YAW_LOCK,
            }[j.name]
            if j.name == "yaw" and j.continuous:
                f |= CAP_SUPPORTS_INFINITE_YAW
        return f

    def send_heartbeat(self) -> None:
        self.send(mavlink.MAVLink_heartbeat_message(MAV_TYPE_GIMBAL, MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE, 3))

    def information_msg(self):
        lim = {j.name: (j.lo, j.hi) for j in self.plant.joints}
        return mavlink.MAVLink_gimbal_device_information_message(
            self.time_boot_ms(),
            encode_str(self.cfg.vendor_name, 32),
            encode_str(self.cfg.model_name, 32),
            encode_str(self.cfg.custom_name, 32),
            version_u32(self.cfg.firmware_version),
            version_u32(self.cfg.hardware_version),
            self.cfg.uid,
            self.cap_flags() & 0xFFFF,
            0,
            lim["roll"][0],
            lim["roll"][1],
            lim["pitch"][0],
            lim["pitch"][1],
            lim["yaw"][0],
            lim["yaw"][1],
            0,
        )

    def send_information(self) -> None:
        self.send(self.information_msg())
        self.info_sent += 1

    def status_msg(self):
        t_ns = time.monotonic_ns()
        q_bc, flags, failure = self.current()
        truth = self._truth(t_ns)
        earth = bool(flags & frames.YAW_IN_EARTH_FRAME)
        q = frames.status_quaternion(truth.q_nb, truth.psi_rad, q_bc, earth)
        t = t_ns / 1e9
        w = (0.0, 0.0, 0.0)
        if self._prev_status_q is not None:
            t0, q0 = self._prev_status_q
            if t - t0 > 1e-3:
                v = to_rotvec(q * q0.conj())
                w = tuple(c / (t - t0) for c in v)
        self._prev_status_q = (t, q)
        p, qq, r = truth.rates
        phi, theta = math.radians(truth.phi_deg), math.radians(truth.theta_deg)
        ct = math.cos(theta) if abs(math.cos(theta)) > 1e-3 else 1e-3
        psi_dot = (qq * math.sin(phi) + r * math.cos(phi)) / ct
        return mavlink.MAVLink_gimbal_device_attitude_status_message(
            0,
            0,
            self.time_boot_ms(),
            flags,
            q.as_list(),
            w[0],
            w[1],
            w[2],
            failure,
            wrap_pi(truth.psi_rad),
            psi_dot,
            0,
        )

    def send_status(self) -> None:
        self.send(self.status_msg())
        self.status_sent += 1

    def _ack(self, msg, command: int, result: int) -> None:
        self.send(
            mavlink.MAVLink_command_ack_message(
                command, result, 0, 0, msg.get_srcSystem(), msg.get_srcComponent()
            )
        )

    def handle(self, msg) -> None:
        mtype = msg.get_type()
        if mtype == "GIMBAL_DEVICE_SET_ATTITUDE":
            self._on_set_attitude(msg)
        elif mtype == "AUTOPILOT_STATE_FOR_GIMBAL_DEVICE" and self.cfg.vehicle_attitude == "autopilot":
            q = Quat(*msg.q)
            if q.is_finite() and q.norm2() > 0.5:
                q = q.normalized()
                y, p, r = to_euler("zyx", q)
                self._autopilot_att = _Level(
                    q_nb=q, psi_rad=y, rates=(0.0, 0.0, msg.angular_velocity_z if math.isfinite(msg.angular_velocity_z) else 0.0),
                    theta_deg=math.degrees(p), phi_deg=math.degrees(r),
                )
        elif mtype in ("COMMAND_LONG", "COMMAND_INT"):
            self._on_command(msg)

    def _on_set_attitude(self, msg) -> None:
        q = Quat(*msg.q)
        raw_rates = (msg.angular_velocity_x, msg.angular_velocity_y, msg.angular_velocity_z)
        rates = tuple(0.0 if math.isnan(v) else v for v in raw_rates)
        rate_mode = all(math.isnan(c) for c in q)
        hold = rate_mode and all(math.isnan(v) for v in raw_rates)
        if not rate_mode and not q.is_finite():
            log.debug("gimbal: ignoring setpoint with partial NaN quaternion")
            return
        with self._lock:
            self.sp = Setpoint(
                flags=msg.flags,
                q=None if rate_mode else q.normalized(),
                rates=rates,
                received_ns=time.monotonic_ns(),
                valid=True,
                hold=hold,
            )
        self.setpoints_rx += 1

    def _on_command(self, msg) -> None:
        cmd = msg.command
        targeted = getattr(msg, "target_component", 0) == self.compid
        if cmd == MAV_CMD_REQUEST_MESSAGE:
            mid = int(msg.param1)
            if mid == MSG_GIMBAL_DEVICE_INFORMATION:
                self._ack(msg, cmd, MAV_RESULT_ACCEPTED)
                self.send_information()
            elif mid == MSG_GIMBAL_DEVICE_ATTITUDE_STATUS:
                self._ack(msg, cmd, MAV_RESULT_ACCEPTED)
                self.send_status()
            elif targeted:
                self._ack(msg, cmd, MAV_RESULT_UNSUPPORTED)
        elif cmd == MAV_CMD_SET_MESSAGE_INTERVAL:
            mid = int(msg.param1)
            if mid == MSG_GIMBAL_DEVICE_ATTITUDE_STATUS:
                us = msg.param2
                if us < 0:
                    self.status_interval_s = 0.0
                elif us == 0:
                    self.status_interval_s = 1.0 / self.cfg.status_rate_hz if self.cfg.status_rate_hz > 0 else 0.0
                else:
                    self.status_interval_s = max(0.005, us / 1e6)
                self._ack(msg, cmd, MAV_RESULT_ACCEPTED)
            elif targeted:
                self._ack(msg, cmd, MAV_RESULT_UNSUPPORTED)
        elif targeted:
            self._ack(msg, cmd, MAV_RESULT_UNSUPPORTED)
