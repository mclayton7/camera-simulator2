"""The SOF-paced CIGI host (HITL.md "CIGI mapping to CamSim", "Timing, sync and KLV").

On each CamSim Start of Frame (UDP 8889) the host predicts the render time
t_r = SOF + one frame period (+ render_offset_s), samples truth and the gimbal
plant at t_r, and sends exactly one datagram about half a frame after the SOF:

  IG Control, Entity Control (camera platform), Platform Kinematics (201),
  View Control (gimbal), and when due: View Definition + Sensor Control (on
  change and every resend_period_s), Celestial (on change and every
  resend_period_s), Atmosphere + Weather (+ Wave Control) together every
  weather.period_s, HAT/HOT Request inside the near-ground band.

Without SOFs (CamSim not up yet) it free-runs at free_run_rate_hz with a warning.
"""

from __future__ import annotations

import logging
import math
import select
import socket
import threading
import time
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Callable

from . import cigi, frames
from .camera import CameraPose, FrameCentre, Optics
from .config import Config, parse_hostport
from .geodesy import Geoid, TerrainBlend, apply_lever_arm
from .quat import Quat, from_euler_zyx
from .timeutil import cigi_date, xp11_utc
from .truth import TruthBuffer, TruthState

log = logging.getLogger(__name__)

INHG_TO_MB = 33.8639


@dataclass
class HostStats:
    sofs: int = 0
    sends: int = 0
    free_run_sends: int = 0
    missed_sofs: int = 0
    late_sends: int = 0
    hat_hot_requests: int = 0
    hat_hot_responses: int = 0
    sensor_ext: int = 0
    bad_responses: int = 0


class CelestialPlanner:
    """What Celestial Sphere Control to send (HITL.md "Time and date").

    system: Date/Time Valid 0, Ephemeris 1 (0 while paused/replaying). After a
      pause CamSim's clock is behind wall time; with resync_after_pause the
      host switches, at the next minute rollover, to Date/Time Valid 1 with the
      system UTC minute, re-sent whenever the minute changes (each change
      re-sets CamSim to hh:mm:00, which is exact at the rollover).
    xplane: scenario UTC from X-Plane's clock (xp11_utc), Date/Time Valid 1 from
      the first minute rollover on, so the first re-set lands on hh:mm:00.
    """

    def __init__(self, source: str = "system", resync_after_pause: bool = True, star_intensity: float = 100.0):
        if source not in ("system", "xplane"):
            raise ValueError(f"time.source must be system or xplane, got {source!r}")
        self.source = source
        self.resync_after_pause = resync_after_pause
        self.star = star_intensity
        self.valid_mode = False
        self._pending_resync = False
        self._was_frozen = False
        self._last_minute: tuple | None = None

    def desired(self, utc: datetime | None, frozen: bool) -> tuple | None:
        """(hour, minute, month, day, year, ephemeris, date_valid) or None (send nothing)."""
        eph = not frozen
        if self._was_frozen and not frozen and self.source == "system" and self.resync_after_pause:
            self._pending_resync = True
        self._was_frozen = frozen
        minute = None
        if utc is not None:
            minute = cigi_date(utc)
            rolled = self._last_minute is not None and minute[:2] != self._last_minute[:2]
            self._last_minute = minute
            if rolled:
                if self.source == "xplane" and not self.valid_mode:
                    self.valid_mode = True
                if self._pending_resync:
                    self._pending_resync = False
                    self.valid_mode = True
        if self.valid_mode and minute is not None:
            return (*minute, eph, True)
        return (0, 0, 0, 0, 0, eph, False)

    def pack(self, d: tuple) -> bytes:
        h, m, mo, dd, y, eph, valid = d
        return cigi.pack_celestial_control(h, m, mo, dd, y, ephemeris=eph, date_valid=valid, star_intensity=self.star)


def coverage_pct(cfg, coverage: float, cloud_type: float) -> float:
    if cfg.coverage_source == "coverage":
        return max(0.0, min(100.0, 100.0 * coverage / max(1e-6, cfg.coverage_scale_max)))
    i = int(round(cloud_type))
    table = cfg.cloud_type_coverage_pct
    return float(table[i]) if 0 <= i < len(table) else 0.0


def dominant_layer(cfg, w) -> tuple[int, float] | None:
    """HITL.md: the dominant layer of the three is the lowest at 50 % or more,
    else the one with the highest coverage. Returns (index, coverage %)."""
    layers = []
    for i in range(3):
        base, tops = w.cloud_base_msl_m[i], w.cloud_tops_msl_m[i]
        pct = coverage_pct(cfg, w.cloud_coverage[i], w.cloud_type[i])
        if pct > 0.0 and tops > base:
            layers.append((i, pct, base))
    if not layers:
        return None
    thick = [lay for lay in layers if lay[1] >= 50.0]
    if thick:
        i, pct, _ = min(thick, key=lambda lay: lay[2])
    else:
        i, pct, _ = max(layers, key=lambda lay: lay[1])
    return i, pct


def build_weather_packets(cfg, w, n_geoid: float) -> bytes:
    """Atmosphere + Weather (+ Wave Control), always together in one datagram."""
    vis = max(cfg.min_visibility_m, w.visibility_m) if w.visibility_m > 0 else 50000.0
    out = cigi.pack_atmosphere_control(
        visibility_m=vis,
        air_temp_c=w.temperature_c,
        humidity_pct=w.humidity_pct,
        wind_speed_ms=w.wind_speed,  # the _kt dataref is really m/s
        wind_dir_deg=w.wind_dir_degt % 360.0,
        baro_mb=w.baro_inhg * INHG_TO_MB if w.baro_inhg > 0 else 1013.25,
    )
    dom = dominant_layer(cfg, w)
    if dom is None:
        out += cigi.pack_weather_control(0.0, 0.0, 0.0, vis)
    else:
        i, pct = dom
        base = w.cloud_base_msl_m[i]
        ct_idx = int(round(w.cloud_type[i]))
        ct = cfg.cloud_type_cigi[ct_idx] if 0 <= ct_idx < len(cfg.cloud_type_cigi) else 0
        out += cigi.pack_weather_control(
            pct, base + n_geoid, w.cloud_tops_msl_m[i] - base, vis, cloud_type=ct
        )
    if cfg.waves and w.wave_length > 0 and w.wave_speed > 0:
        h = w.wave_amplitude * (2.0 if cfg.wave_amplitude_is_half_height else 1.0)
        d = (w.wave_dir + (180.0 if cfg.wave_dir_is_from else 0.0)) % 360.0
        out += cigi.pack_wave_control(h, w.wave_length, w.wave_length / w.wave_speed, d)
    return out


class CigiHost:
    def __init__(
        self,
        cfg: Config,
        truth: TruthBuffer,
        geoid: Geoid,
        gimbal_sample: Callable[[int], Quat] | None = None,
        optics: Callable[[], Optics] | None = None,
    ):
        self.cfg = cfg
        self.c = cfg.cigi
        self.truth = truth
        self.geoid = geoid
        self.gimbal_sample = gimbal_sample
        self.optics_fn = optics
        self.stats = HostStats()
        self.blend = (
            TerrainBlend(
                cfg.terrain_blend.band_low_m,
                cfg.terrain_blend.band_high_m,
                cfg.terrain_blend.correction_tau_s,
                cfg.terrain_blend.hot_max_age_s,
                cfg.terrain_blend.hot_max_distance_m,
            )
            if cfg.terrain_blend.enabled
            else None
        )
        self.celestial = CelestialPlanner(cfg.time.source, cfg.time.resync_after_pause, cfg.time.star_intensity)
        self.view = frames.ViewAngleConverter()
        self.dests = [(self.c.camsim_host, self.c.camsim_port)] + [
            parse_hostport(d, "127.0.0.1") for d in self.c.extra_destinations
        ]
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.rx.bind(parse_hostport(self.c.response_listen))
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="cigi-host", daemon=True)
        self.t0_ns = time.monotonic_ns()
        self.frame_period = 1.0 / max(1.0, self.c.frame_rate)
        self.host_frame = 0
        self.last_ig_frame = 0
        self._last_sof_ns: int | None = None
        self._last_sof_frame: int | None = None
        self._pending: tuple[int, int] | None = None  # (send at ns, t_r ns)
        self._free_running = False
        self._next_free_ns = 0
        self._last_ticks = -1
        self._last_optics: tuple | None = None
        self._last_optics_send = 0.0
        self._last_cel: tuple | None = None
        self._last_cel_send = 0.0
        self._last_weather_send = 0.0
        self._last_hot_req = 0.0
        self._hot_id = 0
        self._hot_reqs: dict[int, tuple[float, float]] = {}
        self._pose: CameraPose | None = None
        self._fc: FrameCentre | None = None
        self._last_stats = time.monotonic()
        self._warned: dict[str, float] = {}
        self.sent_log: list[tuple[int, int, bytes]] | None = None  # tests: (send ns, t_r ns, datagram)

    # -- HostView for the camera ---------------------------------------------
    def latest_pose(self) -> CameraPose | None:
        with self._lock:
            return self._pose

    def latest_frame_centre(self) -> FrameCentre | None:
        with self._lock:
            return self._fc

    def undulation(self, lat: float, lon: float) -> float:
        return self.geoid.undulation(lat, lon)

    @property
    def response_port(self) -> int:
        return self.rx.getsockname()[1]

    # -- lifecycle ---------------------------------------------------------
    def start(self) -> None:
        log.info(
            "cigi: sending to %s, responses on %s:%d",
            ", ".join(f"{h}:{p}" for h, p in self.dests),
            *self.rx.getsockname(),
        )
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        self.tx.close()
        self.rx.close()

    def _warn(self, key: str, msg: str, *args, every: float = 10.0) -> None:
        now = time.monotonic()
        if now - self._warned.get(key, -1e9) >= every:
            self._warned[key] = now
            log.warning(msg, *args)

    # -- main loop ---------------------------------------------------------
    def _run(self) -> None:
        sof_timeout_ns = int(self.c.sof_timeout_s * 1e9)
        free_period_ns = int(1e9 / max(1.0, self.c.free_run_rate_hz))
        while not self._stop.is_set():
            now = time.monotonic_ns()
            if self._pending is not None:
                wait = max(0.0, (self._pending[0] - now) / 1e9)
            elif self._free_running:
                wait = max(0.0, (self._next_free_ns - now) / 1e9)
            else:
                wait = 0.05
            r, _, _ = select.select([self.rx], [], [], min(wait, 0.05))
            if r:
                try:
                    data = self.rx.recv(65535)
                except OSError:
                    if self._stop.is_set():
                        break
                    continue
                self.on_response(data, time.monotonic_ns())
            now = time.monotonic_ns()
            if self._pending is not None and now >= self._pending[0]:
                _, t_r = self._pending
                self._pending = None
                self.send_frame(t_r, now)
            # Free-run fallback when no SOF arrives (bench tests before CamSim is up).
            last = self._last_sof_ns
            if last is None or now - last > sof_timeout_ns:
                if not self._free_running:
                    self._free_running = True
                    self._next_free_ns = now
                    self._warn(
                        "freerun",
                        "cigi: no Start of Frame from CamSim for %.1f s: free-running at %.0f Hz "
                        "(check cigi_response_addr in CamSim's config and response_listen here)",
                        self.c.sof_timeout_s,
                        self.c.free_run_rate_hz,
                        every=30.0,
                    )
                if now >= self._next_free_ns:
                    self._next_free_ns = max(self._next_free_ns + free_period_ns, now - free_period_ns)
                    self.stats.free_run_sends += 1
                    self.send_frame(now + free_period_ns // 2, now)
            if time.monotonic() - self._last_stats >= self.c.stats_period_s:
                self._log_stats()

    def _log_stats(self) -> None:
        self._last_stats = time.monotonic()
        s = self.stats
        latest = self.truth.latest()
        age = (time.monotonic_ns() - latest.arrival_ns) / 1e6 if latest else float("nan")
        log.info(
            "cigi: %s sofs=%d sends=%d free=%d missed=%d late=%d hot=%d/%d fc=%d | truth rx=%d age=%.0f ms%s",
            "FREE-RUN" if self._free_running else "SOF-paced",
            s.sofs,
            s.sends,
            s.free_run_sends,
            s.missed_sofs,
            s.late_sends,
            s.hat_hot_responses,
            s.hat_hot_requests,
            s.sensor_ext,
            self.truth.received,
            age,
            " DILATED" if self.truth.dilated else "",
        )

    # -- responses ---------------------------------------------------------
    def on_response(self, data: bytes, rx_ns: int) -> None:
        try:
            msg = cigi.parse_response_message(data)
        except Exception:
            self.stats.bad_responses += 1
            return
        if msg.sof is not None:
            self.on_sof(msg.sof, rx_ns)
        for hh in msg.hat_hot:
            self.stats.hat_hot_responses += 1
            req = self._hot_reqs.pop(hh.request_id, None)
            if req is not None and hh.valid and hh.hot is not None and self.blend is not None:
                self.blend.update_hot(req[0], req[1], hh.hot, rx_ns / 1e9)
        for se in msg.sensor_ext:
            self.stats.sensor_ext += 1
            with self._lock:
                self._fc = FrameCentre(se.lat, se.lon, se.alt, se.frame, rx_ns)

    def on_sof(self, sof: cigi.StartOfFrame, rx_ns: int) -> None:
        self.stats.sofs += 1
        if self._last_sof_ns is not None:
            dt = (rx_ns - self._last_sof_ns) / 1e9
            if 0.004 < dt < 3.0 * self.frame_period:
                self.frame_period += 0.05 * (dt - self.frame_period)
        if self._last_sof_frame is not None:
            gap = (sof.ig_frame - self._last_sof_frame) & 0xFFFFFFFF
            if 1 < gap < 1000:
                self.stats.missed_sofs += gap - 1
        self._last_sof_frame = sof.ig_frame
        self._last_sof_ns = rx_ns
        self.last_ig_frame = sof.ig_frame
        if self._free_running:
            log.info("cigi: Start of Frame received: SOF-paced")
            self._free_running = False
        if self._pending is not None:
            # The previous SOF's datagram never went out (we were starved): send it now.
            self.stats.late_sends += 1
            _, t_r = self._pending
            self._pending = None
            self.send_frame(t_r, rx_ns)
        period_ns = int(self.frame_period * 1e9)
        send_at = rx_ns + int(self.c.send_phase * period_ns)
        t_r = rx_ns + period_ns + int(self.c.render_offset_s * 1e9)
        self._pending = (send_at, t_r)

    # -- frame assembly ----------------------------------------------------
    def truth_at(self, t_r: int) -> TruthState | None:
        st = self.truth.sample(t_r)
        if st is not None:
            if st.stale:
                self._warn("stale", "cigi: truth is stale (no X-Plane datagram for > %.1f s); holding the last pose", self.cfg.truth.stale_after_s)
            return st
        fb = self.cfg.truth.fallback
        if not fb.enabled:
            self._warn("notruth", "cigi: no truth yet (X-Plane not sending to %s?); sending no Entity Control", self.cfg.truth.listen, every=30.0)
            return None
        q = frames.q_nb_from_euler_deg(fb.heading_deg, fb.pitch_deg, fb.roll_deg)
        return TruthState(
            t_ns=t_r,
            latitude=fb.latitude,
            longitude=fb.longitude,
            elevation=fb.elevation_msl,
            q_nb=q,
            psi_deg=fb.heading_deg % 360.0,
            theta_deg=fb.pitch_deg,
            phi_deg=fb.roll_deg,
            rates=(0.0, 0.0, 0.0),
            vel_ned=(0.0, 0.0, 0.0),
            true_airspeed=0.0,
            indicated_airspeed_ms=0.0,
            groundspeed=0.0,
            mag_psi=fb.heading_deg % 360.0,
            y_agl=1000.0,
            terrain_msl=None,
            paused=False,
            replay=False,
        )

    def _utc_at(self, st: TruthState | None, t_r: int) -> datetime | None:
        if self.cfg.time.source == "system" or st is None or st.source is None:
            return datetime.fromtimestamp(time.time() + (t_r - time.monotonic_ns()) / 1e9, UTC)
        s = st.source
        if not (1 <= s.local_month <= 12 and 1 <= s.local_day <= 31):
            return None
        year = self.cfg.time.local_year or datetime.now().year
        try:
            base = xp11_utc(s.local_time_sec, s.zulu_time_sec, s.local_month, s.local_day, s.longitude, year)
        except ValueError:
            return None
        if st.paused or st.replay:
            return base
        return datetime.fromtimestamp(base.timestamp() + (t_r - s.t_ns) / 1e9, UTC)

    def _optics(self) -> tuple:
        if self.optics_fn is not None:
            o = self.optics_fn()
            return (round(o.hfov, 4), round(o.vfov, 4), o.sensor_id, o.polarity)
        w, h = self.cfg.camera.resolution
        hf = self.c.default_hfov_deg
        return (
            hf,
            cigi.vfov_from_hfov(hf, w, h),
            1 if self.c.default_sensor == "ir" else 0,
            1 if self.c.default_polarity == "black_hot" else 0,
        )

    def _agl(self, st: TruthState) -> float:
        tb = self.cfg.terrain_blend
        if tb.agl_source == "terrain_probe" and st.terrain_msl is not None:
            return st.elevation - st.terrain_msl
        return st.y_agl + tb.y_agl_offset_m

    def build_frame(self, t_r: int, now_ns: int | None = None) -> bytes:
        now_ns = time.monotonic_ns() if now_ns is None else now_ns
        now = now_ns / 1e9
        c = self.c
        ticks = max(self._last_ticks + 1, (t_r - self.t0_ns) // 10_000)
        self._last_ticks = ticks
        out = bytearray(cigi.pack_ig_control(self.host_frame, ticks, self.last_ig_frame))
        self.host_frame += 1

        st = self.truth_at(t_r)
        if self.gimbal_sample is not None:
            q_bc = self.gimbal_sample(t_r)
        else:
            r, p, y = (math.radians(v) for v in c.fixed_view_deg)
            q_bc = from_euler_zyx(y, p, r)
        utc = self._utc_at(st, t_r)

        if st is not None:
            n = self.geoid.undulation(st.latitude, st.longitude)
            alt_ell = st.elevation + n
            agl = self._agl(st)
            if self.blend is not None:
                alt_ell = self.blend.altitude(st.latitude, st.longitude, alt_ell, agl, t_r / 1e9)
            lat, lon, alt = apply_lever_arm(
                st.latitude, st.longitude, alt_ell, st.q_nb, tuple(self.cfg.geodesy.lever_arm_m)
            )
            out += cigi.pack_entity_control(
                c.camera_entity_id, lat, lon, alt, st.psi_deg % 360.0, st.theta_deg, st.phi_deg
            )
            if c.send_platform_kinematics:
                out += cigi.pack_platform_kinematics(
                    c.camera_entity_id,
                    true_airspeed=st.true_airspeed,
                    indicated_airspeed=st.indicated_airspeed_ms,
                    magnetic_heading=st.mag_psi,
                    vel_ned=st.vel_ned,
                    sample_utc=utc.timestamp() if utc is not None else None,
                )
            q_nc = (st.q_nb * q_bc).normalized()
            with self._lock:
                self._pose = CameraPose(
                    t_ns=t_r,
                    lat=lat,
                    lon=lon,
                    alt_msl=alt - self.geoid.undulation(lat, lon),
                    alt_ell=alt,
                    q_nc=q_nc,
                    agl=agl,
                    utc=utc.timestamp() if utc is not None else time.time(),
                )
            # HAT/HOT polling inside the blend band (CamSim has no periodic requests).
            tb = self.cfg.terrain_blend
            if (
                self.blend is not None
                and tb.hot_poll_hz > 0
                and self.blend.in_band(agl)
                and now - self._last_hot_req >= 1.0 / tb.hot_poll_hz
            ):
                self._last_hot_req = now
                self._hot_id = self._hot_id % 0xFFFF + 1
                self._hot_reqs[self._hot_id] = (st.latitude, st.longitude)
                if len(self._hot_reqs) > 32:
                    self._hot_reqs.pop(next(iter(self._hot_reqs)))
                out += cigi.pack_hat_hot_request(self._hot_id, st.latitude, st.longitude, alt_ell)
                self.stats.hat_hot_requests += 1

        va = self.view.convert(q_bc)
        out += cigi.pack_view_control(va.yaw, va.pitch, va.roll, entity_id=c.view_control_entity_id)

        o = self._optics()
        if o != self._last_optics or now - self._last_optics_send >= c.resend_period_s:
            out += cigi.pack_view_definition(o[0], o[1])
            out += cigi.pack_sensor_control(o[2], o[3], sensor_on=True)
            self._last_optics, self._last_optics_send = o, now

        frozen = st is not None and (st.paused or st.replay)
        cel = self.celestial.desired(utc, frozen)
        if cel is not None and (cel != self._last_cel or now - self._last_cel_send >= c.resend_period_s):
            if self._last_cel is not None and cel[5] != self._last_cel[5]:
                log.info("cigi: Celestial Ephemeris Enable -> %d (%s)", cel[5], "frozen" if frozen else "running")
            out += self.celestial.pack(cel)
            self._last_cel, self._last_cel_send = cel, now

        wcfg = self.cfg.weather
        w = self.truth.latest_weather
        if wcfg.enabled and w is not None and st is not None and now - self._last_weather_send >= wcfg.period_s:
            out += build_weather_packets(wcfg, w, self.geoid.undulation(st.latitude, st.longitude))
            self._last_weather_send = now
        return bytes(out)

    def send_frame(self, t_r: int, now_ns: int) -> None:
        try:
            data = self.build_frame(t_r, now_ns)
        except Exception:
            log.exception("cigi: building the frame failed")
            return
        for d in self.dests:
            try:
                self.tx.sendto(data, d)
            except OSError as e:
                self._warn("send", "cigi: send to %s:%d failed: %s", d[0], d[1], e)
        self.stats.sends += 1
        if self.sent_log is not None:
            self.sent_log.append((time.monotonic_ns(), t_r, data))
