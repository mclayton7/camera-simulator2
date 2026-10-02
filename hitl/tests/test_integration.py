"""Localhost end to end: fake_xplane -> IG host -> a fake CamSim that emits SOF
at 30 Hz (little-endian, like CamSim's CCL sender) and answers HAT/HOT."""

import socket
import struct
import threading
import time

import pytest

import fake_xplane
from camsim_hitl import cigi
from camsim_hitl.__main__ import App
from camsim_hitl.config import Config


class FakeCamSim(threading.Thread):
    def __init__(self, rate=30.0, hot_ell=None, send_sof=True):
        super().__init__(daemon=True)
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.bind(("127.0.0.1", 0))
        self.rx.settimeout(0.002)
        self.tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rate = rate
        self.hot_ell = hot_ell
        self.send_sof = send_sof
        self.response_addr = None
        self.stop_ev = threading.Event()
        self.sofs: list[tuple[int, int]] = []  # (frame, t_ns)
        self.got: list[tuple[int, bytes, int]] = []  # (t_ns, data, frame of the last SOF)
        self.pending_hot: list[int] = []

    @property
    def port(self):
        return self.rx.getsockname()[1]

    def run(self):
        period = 1.0 / self.rate
        nxt = time.monotonic()
        frame = 0
        while not self.stop_ev.is_set():
            now = time.monotonic()
            if self.send_sof and self.response_addr and now >= nxt:
                nxt += period
                frame += 1
                d = cigi.pack_sof(frame, order="<")
                for rid in self.pending_hot:
                    d += cigi.pack_hat_hot_ext_response(rid, True, 0.0, self.hot_ell, order="<")
                self.pending_hot.clear()
                d += cigi.pack_sensor_ext_response(frame, 37.62, -122.38, -30.0, order="<")
                self.tx.sendto(d, self.response_addr)
                self.sofs.append((frame, time.monotonic_ns()))
            try:
                data = self.rx.recv(65535)
            except (TimeoutError, socket.timeout):
                continue
            last = self.sofs[-1][0] if self.sofs else 0
            self.got.append((time.monotonic_ns(), data, last))
            if self.hot_ell is not None:
                for op, p in cigi.iter_packets(data):
                    if op == cigi.HAT_HOT_REQUEST:
                        self.pending_hot.append(struct.unpack_from(">H", p, 2)[0])


def make_cfg(camsim_port: int) -> Config:
    c = Config()
    c.truth.listen = "127.0.0.1:0"
    c.cigi.camsim_host = "127.0.0.1"
    c.cigi.camsim_port = camsim_port
    c.cigi.response_listen = "127.0.0.1:0"
    c.cigi.stats_period_s = 1e9
    c.gimbal.enabled = True  # no link: plant only, initial pose
    c.gimbal.initial_deg = [0.0, -30.0, 10.0]
    c.camera.enabled = True
    c.camera.http_port = 0
    c.camera.fov_status_rate_hz = 0.0
    return c


def run_xplane(port, *extra, duration=3.0):
    args = ["--dest", f"127.0.0.1:{port}", "--duration", str(duration), *extra]
    t = threading.Thread(target=fake_xplane.main, args=(args,), daemon=True)
    t.start()
    return t


def parse(data):
    return {op: p for op, p in cigi.iter_packets(data)}, [op for op, _ in cigi.iter_packets(data)]


@pytest.fixture
def setup():
    cs = FakeCamSim()
    app = App(make_cfg(cs.port))
    cs.response_addr = ("127.0.0.1", app.host.response_port)
    yield cs, app
    cs.stop_ev.set()
    app.stop()


def test_one_datagram_per_sof_half_a_frame_later(setup):
    cs, app = setup
    app.start()
    xp = run_xplane(app.truth_rx.port, "orbit", "--lat", "37.62", "--lon", "-122.38", "--alt", "300", duration=3.0)
    cs.start()
    time.sleep(3.0)
    xp.join()
    cs.stop_ev.set()
    cs.join()
    assert app.truth.received > 120  # ~60 Hz for 3 s
    # skip the first half second (free-run fallback before the first SOF)
    t_start = cs.sofs[0][1] + 500_000_000
    sofs = [s for s in cs.sofs if s[1] >= t_start][:-1]
    assert len(sofs) > 60
    per = {f: [] for f, _ in sofs}
    for t, data, f in cs.got:
        if f in per:
            per[f].append((t, data))
    counts = [len(v) for v in per.values()]
    assert counts.count(1) >= len(counts) - 1, counts  # exactly one datagram per SOF
    sof_t = dict(sofs)
    delays = sorted((v[0][0] - sof_t[f]) / 1e6 for f, v in per.items() if v)
    med = delays[len(delays) // 2]
    assert 12.0 < med < 22.0, delays  # ~half of a 33 ms frame
    # contents
    f0 = next(iter(per))
    pk, ops = parse(per[f0][0][1])
    assert ops[0] == cigi.IG_CONTROL and ops[1] == cigi.ENTITY_CONTROL and ops[2] == cigi.PLATFORM_KINEMATICS
    assert cigi.VIEW_CONTROL in ops
    assert pk[cigi.IG_CONTROL][6:8] == b"\x80\x00"
    last_ig = struct.unpack_from(">I", pk[cigi.IG_CONTROL], 16)[0]
    assert last_ig == f0  # Last Received IG Frame echoes the SOF
    lat, lon, alt = struct.unpack_from(">ddd", pk[cigi.ENTITY_CONTROL], 24)
    n = app.geoid.undulation(37.62, -122.38)
    assert abs(lat - 37.62) < 0.01 and abs(lon + 122.38) < 0.01
    assert alt == pytest.approx(300.0 + n, abs=0.5)  # MSL + EGM96 = ellipsoid
    roll, pitch, yaw = struct.unpack_from(">fff", pk[cigi.ENTITY_CONTROL], 12)
    assert 0 <= yaw < 360 and roll > 0  # clockwise orbit banks right
    r, p, y = struct.unpack_from(">fff", pk[cigi.VIEW_CONTROL], 20)
    assert -180 <= y < 180 and p == pytest.approx(-30.0, abs=0.5) and y == pytest.approx(10.0, abs=0.5)
    kin = pk[cigi.PLATFORM_KINEMATICS]
    assert kin[4] == 0x0F
    utc = struct.unpack_from(">d", kin, 32)[0]
    assert abs(utc - time.time()) < 10.0
    # timestamps strictly increase
    ts = [struct.unpack_from(">I", parse(d)[0][cigi.IG_CONTROL], 12)[0] for _, d, _ in cs.got]
    assert all(b > a for a, b in zip(ts, ts[1:]))
    # periodic packets over the run
    allops = [op for _, d, _ in cs.got for op, _ in cigi.iter_packets(d)]
    assert allops.count(cigi.VIEW_DEFINITION) >= 2 and allops.count(cigi.SENSOR_CONTROL) >= 2
    assert allops.count(cigi.CELESTIAL_CONTROL) >= 2
    assert allops.count(cigi.ATMOSPHERE_CONTROL) >= 2
    assert allops.count(cigi.ATMOSPHERE_CONTROL) == allops.count(cigi.WEATHER_CONTROL)  # always together
    for _, d, _ in cs.got:
        pk, ops = parse(d)
        if cigi.CELESTIAL_CONTROL in pk:
            assert pk[cigi.CELESTIAL_CONTROL][4] & 0x11 == 0x01  # ephemeris on, date/time not valid
        if cigi.WEATHER_CONTROL in pk:
            base = struct.unpack_from(">f", pk[cigi.WEATHER_CONTROL], 24)[0]
            assert base == pytest.approx(1500.0 + n, abs=1.0)  # cloud base + N_geoid
    # the camera saw the frame centre
    assert app.host.latest_frame_centre() is not None
    assert app.host.latest_pose() is not None


def test_free_run_without_sof():
    cs = FakeCamSim(send_sof=False)
    cfg = make_cfg(cs.port)
    cfg.cigi.free_run_rate_hz = 20.0
    app = App(cfg)
    cs.response_addr = ("127.0.0.1", app.host.response_port)
    app.start()
    cs.start()
    try:
        time.sleep(1.0)
    finally:
        cs.stop_ev.set()
        app.stop()
    assert 15 <= len(cs.got) <= 25  # ~20 Hz with no CamSim
    _, ops = parse(cs.got[-1][1])
    assert ops[0] == cigi.IG_CONTROL and cigi.VIEW_CONTROL in ops
    assert cigi.ENTITY_CONTROL not in ops  # no truth, no fallback pose


def test_near_ground_blend_uses_camsim_hot():
    cs = FakeCamSim(hot_ell=10.0)
    cfg = make_cfg(cs.port)
    cfg.terrain_blend.hot_poll_hz = 5.0
    cfg.terrain_blend.correction_tau_s = 0.1
    app = App(cfg)
    cs.response_addr = ("127.0.0.1", app.host.response_port)
    app.start()
    xp = run_xplane(app.truth_rx.port, "parked", "--lat", "37.62", "--lon", "-122.38", "--alt", "60", "--agl", "3", duration=2.5)
    cs.start()
    try:
        time.sleep(2.5)
        xp.join()
    finally:
        cs.stop_ev.set()
        app.stop()
    reqs = [d for _, d, _ in cs.got if cigi.HAT_HOT_REQUEST in parse(d)[0]]
    assert 6 <= len(reqs) <= 16  # 5 Hz polling inside the band
    pk, _ = parse(cs.got[-1][1])
    alt = struct.unpack_from(">d", pk[cigi.ENTITY_CONTROL], 40)[0]
    assert alt == pytest.approx(13.0, abs=0.2)  # Cesium HOT 10 m + y_agl 3 m, not 60 + N
