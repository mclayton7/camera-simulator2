"""Celestial, weather and frame-assembly rules of the CIGI host (no sockets needed
beyond the host's own)."""

import struct
from datetime import UTC, datetime, timedelta

import pytest

from camsim_hitl import cigi
from camsim_hitl.cigi_host import CelestialPlanner, CigiHost, build_weather_packets, dominant_layer
from camsim_hitl.config import Config, WeatherConfig
from camsim_hitl.geodesy import Geoid
from camsim_hitl.truth import TruthBuffer, Weather

T0 = datetime(2026, 12, 31, 23, 58, 30, tzinfo=UTC)


def test_celestial_system_time_and_pause():
    c = CelestialPlanner("system")
    d = c.desired(T0, frozen=False)
    assert d == (0, 0, 0, 0, 0, True, False)  # Date/Time Valid 0, ephemeris running
    assert c.desired(T0, frozen=True)[5] is False  # paused: freeze CamSim's clock
    # resume: still not valid until the next minute rollover, then re-set at hh:mm:00
    assert c.desired(T0 + timedelta(seconds=5), frozen=False)[6] is False
    d = c.desired(T0 + timedelta(seconds=31), frozen=False)  # 23:59:01
    assert d == (23, 59, 12, 31, 2026, True, True)
    d = c.desired(T0 + timedelta(seconds=91), frozen=False)  # 00:00:01 next year
    assert d == (0, 0, 1, 1, 2027, True, True)


def test_celestial_scenario_waits_for_minute_rollover():
    c = CelestialPlanner("xplane")
    assert c.desired(T0, False)[6] is False
    assert c.desired(T0 + timedelta(seconds=20), False)[6] is False
    d = c.desired(T0 + timedelta(seconds=30), False)  # 23:59:00 exactly
    assert d == (23, 59, 12, 31, 2026, True, True)
    p = c.pack(d)
    assert p[4] == 0x1F and struct.unpack(">I", p[8:12])[0] == 12312026


def wx(**kw):
    w = Weather(visibility_m=8000.0, temperature_c=12.0, humidity_pct=70.0, wind_speed=4.0, wind_dir_degt=200.0,
                baro_inhg=29.92, cloud_base_msl_m=(800.0, 2000.0, 6000.0), cloud_tops_msl_m=(1000.0, 2600.0, 6400.0),
                cloud_coverage=(2.0, 5.0, 1.0), cloud_type=(2.0, 4.0, 1.0))
    for k, v in kw.items():
        setattr(w, k, v)
    return w


def test_dominant_layer_rule():
    cfg = WeatherConfig()
    # 0: scattered 40 %, 1: overcast 100 % at 2000 m, 2: cirrus 20 %: lowest >= 50 % wins
    assert dominant_layer(cfg, wx()) == (1, 100.0)
    # two layers >= 50 %: the lowest
    assert dominant_layer(cfg, wx(cloud_type=(3.0, 4.0, 1.0)))[0] == 0
    # none >= 50 %: the highest coverage
    assert dominant_layer(cfg, wx(cloud_type=(2.0, 1.0, 1.0)))[0] == 0
    # coverage scale source
    cfg2 = WeatherConfig(coverage_source="coverage", coverage_scale_max=6.0)
    assert dominant_layer(cfg2, wx()) == (1, pytest.approx(500 / 6))
    assert dominant_layer(cfg, wx(cloud_type=(0.0, 0.0, 0.0))) is None


def test_weather_packets_atmosphere_weather_wave_together():
    cfg = WeatherConfig()
    w = wx(wave_amplitude=0.5, wave_length=30.0, wave_speed=6.0, wave_dir=90.0)
    d = build_weather_packets(cfg, w, n_geoid=-32.0)
    ops = [op for op, _ in cigi.iter_packets(d)]
    assert ops == [cigi.ATMOSPHERE_CONTROL, cigi.WEATHER_CONTROL, cigi.WAVE_CONTROL]
    pk = dict(cigi.iter_packets(d))
    a = pk[cigi.ATMOSPHERE_CONTROL]
    assert a[3] == 70 and struct.unpack(">ff", a[4:12]) == (12.0, 8000.0)
    assert struct.unpack(">f", a[24:28])[0] == pytest.approx(29.92 * 33.8639, rel=1e-6)
    wc = pk[cigi.WEATHER_CONTROL]
    cov, base, thick = struct.unpack(">fff", wc[20:32])
    assert (cov, base, thick) == (100.0, 2000.0 - 32.0, 600.0)  # base + N_geoid
    assert struct.unpack(">f", wc[12:16])[0] == 8000.0  # same visibility as Atmosphere
    h, length, period, direction = struct.unpack(">ffff", pk[cigi.WAVE_CONTROL][8:24])
    assert (h, length, period, direction) == (1.0, 30.0, 5.0, 90.0)


@pytest.fixture
def host():
    cfg = Config()
    cfg.cigi.response_listen = "127.0.0.1:0"
    cfg.truth.fallback.enabled = True
    cfg.truth.fallback.elevation_msl = 500.0
    h = CigiHost(cfg, TruthBuffer(), Geoid(allow_missing=True))
    yield h
    h.tx.close()
    h.rx.close()


def test_frame_with_fallback_pose_and_resend_rules(host):
    host.t0_ns = 0  # synthetic times below
    d1 = host.build_frame(10_000_000_000, 10_000_000_000)
    ops1 = [op for op, _ in cigi.iter_packets(d1)]
    assert ops1[:4] == [cigi.IG_CONTROL, cigi.ENTITY_CONTROL, cigi.PLATFORM_KINEMATICS, cigi.VIEW_CONTROL]
    assert cigi.VIEW_DEFINITION in ops1 and cigi.SENSOR_CONTROL in ops1 and cigi.CELESTIAL_CONTROL in ops1
    # next frame: nothing changed, nothing due
    d2 = host.build_frame(10_033_000_000, 10_033_000_000)
    ops2 = [op for op, _ in cigi.iter_packets(d2)]
    assert ops2 == [cigi.IG_CONTROL, cigi.ENTITY_CONTROL, cigi.PLATFORM_KINEMATICS, cigi.VIEW_CONTROL]
    # a second later they are re-sent
    d3 = host.build_frame(11_100_000_000, 11_100_000_000)
    assert cigi.VIEW_DEFINITION in [op for op, _ in cigi.iter_packets(d3)]
    # host frame number and timestamp
    f1, ts1 = struct.unpack(">II", d1[8:16])
    f2, ts2 = struct.unpack(">II", d2[8:16])
    assert f2 == f1 + 1 and ts2 - ts1 == 3300  # 33 ms in 10 us ticks
    # View Control from the fixed gimbal setting, Entity ID 0
    vc = dict(cigi.iter_packets(d2))[cigi.VIEW_CONTROL]
    assert struct.unpack(">H", vc[6:8])[0] == 0
    assert struct.unpack(">f", vc[24:28])[0] == pytest.approx(-30.0)
    # the optics change triggers View Definition at once
    host.c.default_hfov_deg = 20.0
    d4 = host.build_frame(11_133_000_000, 11_133_000_000)
    vd = dict(cigi.iter_packets(d4))[cigi.VIEW_DEFINITION]
    assert struct.unpack(">ff", vd[16:24]) == (-10.0, 10.0)
