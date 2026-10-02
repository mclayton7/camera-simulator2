"""X-Plane truth: the CSTR datagram (hitl/PROTOCOL.md section 1), a ring buffer,
interpolation and short extrapolation to a requested host time.

Times are host ``time.monotonic_ns()``. Each sample is stamped on arrival; when
``use_sender_clock`` is on, the stamp is the sender's own monotonic clock mapped
onto the host clock with a running minimum of (arrival - sender) offsets, which
removes network/scheduling jitter from the sample spacing (the minimum is the
least-delayed datagram; the window lets it follow clock drift).
"""

from __future__ import annotations

import collections
import logging
import math
import socket
import struct
import threading
import time
from dataclasses import dataclass, field, replace

from .geodesy import offset_ned
from .quat import Quat, from_rotvec, slerp, to_euler_zyx
from .frames import q_nb_from_euler_deg

log = logging.getLogger(__name__)

MAGIC = b"CSTR"
VERSION = 1
TRUTH_STRUCT = struct.Struct("<4sHHIIQdddd" + "f" * 18 + "BBBB" + "f" * 23)
TRUTH_SIZE = 224
assert TRUTH_STRUCT.size == TRUTH_SIZE

FLAG_PAUSED = 1
FLAG_REPLAY = 2
FLAG_WEATHER = 4
FLAG_TERRAIN = 8

KNOTS_TO_MS = 0.514444


@dataclass
class Weather:
    visibility_m: float = 0.0
    temperature_c: float = 0.0
    humidity_pct: float = 0.0
    wind_speed: float = 0.0  # raw sim/weather/wind_speed_kt (actually m/s)
    wind_dir_degt: float = 0.0
    baro_inhg: float = 0.0
    cloud_base_msl_m: tuple[float, float, float] = (0.0, 0.0, 0.0)
    cloud_tops_msl_m: tuple[float, float, float] = (0.0, 0.0, 0.0)
    cloud_coverage: tuple[float, float, float] = (0.0, 0.0, 0.0)
    cloud_type: tuple[float, float, float] = (0.0, 0.0, 0.0)
    wave_amplitude: float = 0.0
    wave_length: float = 0.0
    wave_speed: float = 0.0
    wave_dir: float = 0.0


@dataclass
class TruthSample:
    """One CSTR datagram, fields named after PROTOCOL.md."""

    flags: int
    cycle: int
    seq: int
    mono_ns: int
    latitude: float
    longitude: float
    elevation: float
    terrain_msl: float
    true_psi: float
    true_theta: float
    true_phi: float
    p: float
    q: float
    r: float
    local_vx: float
    local_vy: float
    local_vz: float
    true_airspeed: float
    groundspeed: float
    indicated_airspeed: float  # knots
    mag_psi: float
    y_agl: float
    sim_speed: float
    sim_speed_actual_ogl: float
    zulu_time_sec: float
    local_time_sec: float
    local_month: int
    local_day: int
    use_system_time: int
    earth_radius_m: float
    weather: Weather | None
    t_ns: int = 0  # host time this sample represents (filled by the buffer)
    arrival_ns: int = 0

    @property
    def paused(self) -> bool:
        return bool(self.flags & FLAG_PAUSED)

    @property
    def replay(self) -> bool:
        return bool(self.flags & FLAG_REPLAY)

    @property
    def terrain_valid(self) -> bool:
        return bool(self.flags & FLAG_TERRAIN)

    def vel_ned(self) -> tuple[float, float, float]:
        """OpenGL (east, up, south) -> NED. Exact at X-Plane's OpenGL origin;
        good enough for a frame or two of extrapolation (HITL.md "Pitfalls")."""
        return (-self.local_vz, self.local_vx, -self.local_vy)


def parse_truth(data: bytes) -> TruthSample:
    """Parse one datagram; raises ValueError on bad magic, version or size."""
    if len(data) != TRUTH_SIZE:
        raise ValueError(f"truth datagram is {len(data)} bytes, expected {TRUTH_SIZE}")
    v = TRUTH_STRUCT.unpack(data)
    if v[0] != MAGIC:
        raise ValueError(f"bad truth magic {v[0]!r}")
    if v[1] != VERSION:
        raise ValueError(f"unsupported truth version {v[1]}")
    flags = v[2]
    f = v[10:28]  # 18 floats from offset 56
    b = v[28:32]
    g = v[32:]  # 23 floats from offset 132
    weather = None
    if flags & FLAG_WEATHER:
        weather = Weather(
            visibility_m=g[1],
            temperature_c=g[2],
            humidity_pct=g[3],
            wind_speed=g[4],
            wind_dir_degt=g[5],
            baro_inhg=g[6],
            cloud_base_msl_m=tuple(g[7:10]),
            cloud_tops_msl_m=tuple(g[10:13]),
            cloud_coverage=tuple(g[13:16]),
            cloud_type=tuple(g[16:19]),
            wave_amplitude=g[19],
            wave_length=g[20],
            wave_speed=g[21],
            wave_dir=g[22],
        )
    return TruthSample(
        flags=flags,
        cycle=v[3],
        seq=v[4],
        mono_ns=v[5],
        latitude=v[6],
        longitude=v[7],
        elevation=v[8],
        terrain_msl=v[9],
        true_psi=f[0],
        true_theta=f[1],
        true_phi=f[2],
        p=f[3],
        q=f[4],
        r=f[5],
        local_vx=f[6],
        local_vy=f[7],
        local_vz=f[8],
        true_airspeed=f[9],
        groundspeed=f[10],
        indicated_airspeed=f[11],
        mag_psi=f[12],
        y_agl=f[13],
        sim_speed=f[14],
        sim_speed_actual_ogl=f[15],
        zulu_time_sec=f[16],
        local_time_sec=f[17],
        local_month=b[0],
        local_day=b[1],
        use_system_time=b[2],
        earth_radius_m=g[0],
        weather=weather,
    )


def pack_truth(s: TruthSample) -> bytes:
    """Inverse of parse_truth (used by tools/fake_xplane.py and tests)."""
    w = s.weather or Weather()
    flags = (s.flags | FLAG_WEATHER) if s.weather is not None else (s.flags & ~FLAG_WEATHER)
    wvals = (
        [w.visibility_m, w.temperature_c, w.humidity_pct, w.wind_speed, w.wind_dir_degt, w.baro_inhg]
        + list(w.cloud_base_msl_m)
        + list(w.cloud_tops_msl_m)
        + list(w.cloud_coverage)
        + list(w.cloud_type)
        + [w.wave_amplitude, w.wave_length, w.wave_speed, w.wave_dir]
    )
    if s.weather is None:
        wvals = [0.0] * len(wvals)
    return TRUTH_STRUCT.pack(
        MAGIC,
        VERSION,
        flags,
        s.cycle & 0xFFFFFFFF,
        s.seq & 0xFFFFFFFF,
        s.mono_ns & 0xFFFFFFFFFFFFFFFF,
        s.latitude,
        s.longitude,
        s.elevation,
        s.terrain_msl,
        s.true_psi,
        s.true_theta,
        s.true_phi,
        s.p,
        s.q,
        s.r,
        s.local_vx,
        s.local_vy,
        s.local_vz,
        s.true_airspeed,
        s.groundspeed,
        s.indicated_airspeed,
        s.mag_psi,
        s.y_agl,
        s.sim_speed,
        s.sim_speed_actual_ogl,
        s.zulu_time_sec,
        s.local_time_sec,
        s.local_month,
        s.local_day,
        s.use_system_time,
        0,
        s.earth_radius_m,
        *wvals,
    )


@dataclass
class TruthState:
    """Truth resampled to a host time."""

    t_ns: int
    latitude: float
    longitude: float
    elevation: float  # m MSL
    q_nb: Quat
    psi_deg: float
    theta_deg: float
    phi_deg: float
    rates: tuple[float, float, float]  # body p, q, r rad/s
    vel_ned: tuple[float, float, float]
    true_airspeed: float
    indicated_airspeed_ms: float
    groundspeed: float
    mag_psi: float
    y_agl: float
    terrain_msl: float | None
    paused: bool
    replay: bool
    extrapolated_s: float = 0.0
    stale: bool = False
    source: TruthSample | None = field(default=None, repr=False)

    @property
    def psi_rad(self) -> float:
        return math.radians(self.psi_deg)


def _lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def _lerp_lon(a: float, b: float, t: float) -> float:
    d = (b - a + 180.0) % 360.0 - 180.0
    return (a + d * t + 180.0) % 360.0 - 180.0


def _state_from_sample(s: TruthSample) -> TruthState:
    q = q_nb_from_euler_deg(s.true_psi, s.true_theta, s.true_phi)
    return TruthState(
        t_ns=s.t_ns,
        latitude=s.latitude,
        longitude=s.longitude,
        elevation=s.elevation,
        q_nb=q,
        psi_deg=s.true_psi % 360.0,
        theta_deg=s.true_theta,
        phi_deg=s.true_phi,
        rates=(s.p, s.q, s.r),
        vel_ned=s.vel_ned(),
        true_airspeed=s.true_airspeed,
        indicated_airspeed_ms=s.indicated_airspeed * KNOTS_TO_MS,
        groundspeed=s.groundspeed,
        mag_psi=s.mag_psi % 360.0,
        y_agl=s.y_agl,
        terrain_msl=s.terrain_msl if s.terrain_valid else None,
        paused=s.paused,
        replay=s.replay,
        source=s,
    )


def _set_attitude(st: TruthState, q: Quat) -> TruthState:
    y, p, r = to_euler_zyx(q)
    st.q_nb = q
    st.psi_deg = math.degrees(y) % 360.0
    st.theta_deg = math.degrees(p)
    st.phi_deg = math.degrees(r)
    return st


class TruthBuffer:
    """Thread-safe ring of truth samples with resampling to a host time."""

    def __init__(
        self,
        capacity: int = 256,
        max_extrapolation_s: float = 0.1,
        stale_after_s: float = 0.5,
        use_sender_clock: bool = True,
        clock_window: int = 240,
        replay_policy: str = "freeze",
        sim_speed_warn_below: float = 0.98,
    ):
        self._lock = threading.Lock()
        self._ring: collections.deque[TruthSample] = collections.deque(maxlen=capacity)
        self._offsets: collections.deque[int] = collections.deque(maxlen=clock_window)
        self.max_extrapolation_ns = int(max_extrapolation_s * 1e9)
        self.stale_after_ns = int(stale_after_s * 1e9)
        self.use_sender_clock = use_sender_clock
        self.replay_policy = replay_policy
        self.sim_speed_warn_below = sim_speed_warn_below
        self._frozen: TruthSample | None = None  # last live sample before replay
        self.latest_weather: Weather | None = None
        self.latest_weather_ns = 0
        self.received = 0
        self.rejected = 0
        self.out_of_order = 0
        self._last_dilation_warn = 0.0
        self.dilated = False

    # -- ingest ------------------------------------------------------------
    def add(self, s: TruthSample, arrival_ns: int | None = None) -> bool:
        """Insert a sample; returns False if it was dropped (duplicate/out of order)."""
        arrival_ns = time.monotonic_ns() if arrival_ns is None else arrival_ns
        s.arrival_ns = arrival_ns
        with self._lock:
            last = self._ring[-1] if self._ring else None
            if last is not None:
                dseq = (s.seq - last.seq) & 0xFFFFFFFF
                if dseq == 0 or dseq > 0x7FFFFFFF:
                    # Older or duplicate. A big backwards jump is a plugin restart.
                    back = (last.seq - s.seq) & 0xFFFFFFFF
                    if back > 1000 or s.mono_ns + 5_000_000_000 < last.mono_ns:
                        log.warning("truth sequence restarted (seq %d -> %d): reset", last.seq, s.seq)
                        self._ring.clear()
                        self._offsets.clear()
                    else:
                        self.out_of_order += 1
                        return False
            if self.use_sender_clock and s.mono_ns:
                if self._ring and s.mono_ns < self._ring[-1].mono_ns:
                    self._ring.clear()
                    self._offsets.clear()
                self._offsets.append(arrival_ns - s.mono_ns)
                s.t_ns = s.mono_ns + min(self._offsets)
            else:
                s.t_ns = arrival_ns
            if self._ring and s.t_ns <= self._ring[-1].t_ns:
                s.t_ns = self._ring[-1].t_ns + 1
            self._ring.append(s)
            self.received += 1
            if s.weather is not None:
                self.latest_weather = s.weather
                self.latest_weather_ns = arrival_ns
            if not s.replay:
                self._frozen = s
        self._check_dilation(s)
        return True

    def _check_dilation(self, s: TruthSample) -> None:
        if s.paused or s.replay:
            return
        dil = 0.0 < s.sim_speed_actual_ogl < self.sim_speed_warn_below
        self.dilated = dil
        if dil:
            now = time.monotonic()
            if now - self._last_dilation_warn > 5.0:
                self._last_dilation_warn = now
                log.warning(
                    "X-Plane time dilation: sim_speed_actual_ogl = %.3f (< %.2f); "
                    "flag this run (HITL.md Pitfalls)",
                    s.sim_speed_actual_ogl,
                    self.sim_speed_warn_below,
                )

    # -- queries -----------------------------------------------------------
    def latest(self) -> TruthSample | None:
        with self._lock:
            return self._ring[-1] if self._ring else None

    def __len__(self) -> int:
        with self._lock:
            return len(self._ring)

    def sample(self, t_ns: int) -> TruthState | None:
        """Truth at host time t_ns: interpolate, or extrapolate a short way."""
        with self._lock:
            if not self._ring:
                return None
            newest = self._ring[-1]
            if newest.replay and self.replay_policy == "freeze":
                base = self._frozen
                if base is None:
                    return None
                st = _state_from_sample(base)
                st.replay = True
                st.vel_ned = (0.0, 0.0, 0.0)
                st.rates = (0.0, 0.0, 0.0)
                st.t_ns = t_ns
                return st
            if newest.paused:
                st = _state_from_sample(newest)
                st.vel_ned = (0.0, 0.0, 0.0)
                st.rates = (0.0, 0.0, 0.0)
                st.t_ns = t_ns
                return st
            if t_ns >= newest.t_ns:
                return self._extrapolate(newest, t_ns)
            # Find the bracketing pair (ring is short; scan from the end).
            ring = self._ring
            older = None
            for i in range(len(ring) - 1, -1, -1):
                if ring[i].t_ns <= t_ns:
                    older = ring[i]
                    newer = ring[i + 1]
                    break
            if older is None:
                st = _state_from_sample(ring[0])
                st.t_ns = t_ns
                return st
        a = _state_from_sample(older)
        b = _state_from_sample(newer)
        u = (t_ns - older.t_ns) / max(1, newer.t_ns - older.t_ns)
        st = replace(
            b,
            t_ns=t_ns,
            latitude=_lerp(a.latitude, b.latitude, u),
            longitude=_lerp_lon(a.longitude, b.longitude, u),
            elevation=_lerp(a.elevation, b.elevation, u),
            rates=tuple(_lerp(x, y, u) for x, y in zip(a.rates, b.rates)),
            vel_ned=tuple(_lerp(x, y, u) for x, y in zip(a.vel_ned, b.vel_ned)),
            true_airspeed=_lerp(a.true_airspeed, b.true_airspeed, u),
            indicated_airspeed_ms=_lerp(a.indicated_airspeed_ms, b.indicated_airspeed_ms, u),
            groundspeed=_lerp(a.groundspeed, b.groundspeed, u),
            y_agl=_lerp(a.y_agl, b.y_agl, u),
        )
        return _set_attitude(st, slerp(a.q_nb, b.q_nb, u).normalized())

    def _extrapolate(self, s: TruthSample, t_ns: int) -> TruthState:
        st = _state_from_sample(s)
        dt_ns = t_ns - s.t_ns
        st.stale = dt_ns > self.stale_after_ns
        dt = min(dt_ns, self.max_extrapolation_ns) / 1e9
        st.t_ns = t_ns
        st.extrapolated_s = dt
        if dt <= 0.0:
            return st
        vn, ve, vd = st.vel_ned
        st.latitude, st.longitude, st.elevation = offset_ned(
            st.latitude, st.longitude, st.elevation, vn * dt, ve * dt, vd * dt
        )
        st.y_agl = st.y_agl - vd * dt
        p, q, r = st.rates
        return _set_attitude(st, (st.q_nb * from_rotvec((p * dt, q * dt, r * dt))).normalized())


class TruthReceiver(threading.Thread):
    """UDP listener feeding a TruthBuffer."""

    def __init__(self, buffer: TruthBuffer, host: str = "0.0.0.0", port: int = 49300):
        super().__init__(name="truth-rx", daemon=True)
        self.buffer = buffer
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.settimeout(0.25)
        self._halt = threading.Event()
        self._bad_logged = 0.0

    @property
    def port(self) -> int:
        return self.sock.getsockname()[1]

    def stop(self) -> None:
        self._halt.set()

    def run(self) -> None:
        log.info("truth: listening on %s:%d", *self.sock.getsockname())
        while not self._halt.is_set():
            try:
                data, _ = self.sock.recvfrom(2048)
            except (TimeoutError, socket.timeout):
                continue
            except OSError:
                break
            now = time.monotonic_ns()
            try:
                s = parse_truth(data)
            except ValueError as e:
                self.buffer.rejected += 1
                if time.monotonic() - self._bad_logged > 5.0:
                    self._bad_logged = time.monotonic()
                    log.warning("truth: rejected datagram: %s", e)
                continue
            self.buffer.add(s, now)
        self.sock.close()
