import math
import struct
from datetime import UTC, datetime
from types import SimpleNamespace

import pytest

import fake_xplane
from camsim_hitl.geodesy import ned_distance
from camsim_hitl.timeutil import xp11_utc
from camsim_hitl.truth import (
    FLAG_PAUSED,
    FLAG_REPLAY,
    TRUTH_SIZE,
    TruthBuffer,
    TruthSample,
    Weather,
    pack_truth,
    parse_truth,
)

MS = 1_000_000


def sample(seq, t_ms, lat=37.0, lon=-122.0, elev=100.0, psi=0.0, theta=0.0, phi=0.0,
           p=0.0, q=0.0, r=0.0, vx=0.0, vy=0.0, vz=0.0, flags=0, weather=None):
    return TruthSample(
        flags=flags, cycle=seq, seq=seq, mono_ns=t_ms * MS, latitude=lat, longitude=lon,
        elevation=elev, terrain_msl=0.0, true_psi=psi, true_theta=theta, true_phi=phi,
        p=p, q=q, r=r, local_vx=vx, local_vy=vy, local_vz=vz, true_airspeed=30.0,
        groundspeed=29.0, indicated_airspeed=55.0, mag_psi=350.0, y_agl=100.0,
        sim_speed=1.0, sim_speed_actual_ogl=1.0, zulu_time_sec=3600.0, local_time_sec=3600.0 - 28800,
        local_month=6, local_day=1, use_system_time=1, earth_radius_m=6378145.0, weather=weather,
    )


def test_struct_matches_protocol_md():
    # PROTOCOL.md section 1: offsets of a few fields, little-endian, 224 bytes
    s = sample(7, 0, lat=12.5, lon=-45.25, elev=321.0, psi=10.0)
    s.terrain_msl = 300.0
    s.flags = FLAG_PAUSED
    d = pack_truth(s)
    assert len(d) == TRUTH_SIZE == 224
    assert d[0:4] == b"CSTR"
    assert struct.unpack_from("<H", d, 4)[0] == 1
    assert struct.unpack_from("<H", d, 6)[0] == FLAG_PAUSED
    assert struct.unpack_from("<I", d, 12)[0] == 7
    assert struct.unpack_from("<d", d, 24)[0] == 12.5
    assert struct.unpack_from("<d", d, 32)[0] == -45.25
    assert struct.unpack_from("<d", d, 40)[0] == 321.0
    assert struct.unpack_from("<d", d, 48)[0] == 300.0
    assert struct.unpack_from("<f", d, 56)[0] == 10.0
    assert struct.unpack_from("<f", d, 100)[0] == 55.0  # indicated_airspeed, knots
    assert struct.unpack_from("<f", d, 108)[0] == 100.0  # y_agl
    assert d[128] == 6 and d[129] == 1 and d[130] == 1
    assert struct.unpack_from("<f", d, 132)[0] == pytest.approx(6378145.0)


def test_round_trip_with_fake_xplane_packer():
    args = SimpleNamespace(scenario="orbit", lat=37.62, lon=-122.38, alt=300.0, agl=290.0, speed=25.0,
                           radius=400.0, heading=0.0, pitch=0.0, tz_hours=-8, mag_var=13.0,
                           visibility=9000.0, cloud_base=1500.0, cloud_type=3)
    scen = fake_xplane.Scenario(args)
    s = fake_xplane.make_sample(args, scen, 3.0, 42, with_weather=True)
    d = pack_truth(s)
    r = parse_truth(d)
    assert r.seq == 42 and r.cycle == 42
    assert r.latitude == s.latitude and r.longitude == s.longitude and r.elevation == s.elevation
    assert r.true_psi == pytest.approx(s.true_psi, abs=1e-4)
    assert r.true_phi == pytest.approx(s.true_phi, abs=1e-4)
    assert r.weather is not None and r.weather.visibility_m == pytest.approx(9000.0)
    assert r.weather.cloud_type[0] == 3.0
    assert r.weather.cloud_base_msl_m[0] == 1500.0
    assert r.weather.wave_length == pytest.approx(30.0)
    # without weather the block is zero and flag bit2 clear
    s2 = fake_xplane.make_sample(args, scen, 3.0, 43, with_weather=False)
    r2 = parse_truth(pack_truth(s2))
    assert r2.weather is None and r2.flags & 4 == 0
    # orbit: bank to the right for a clockwise turn, velocity is tangent
    vn, ve, vd = r.vel_ned()
    assert r.true_phi > 0
    assert math.degrees(math.atan2(ve, vn)) % 360 == pytest.approx(r.true_psi % 360, abs=1e-3)


def test_parse_rejects_bad_datagrams():
    d = pack_truth(sample(1, 0))
    with pytest.raises(ValueError):
        parse_truth(d[:-1])
    with pytest.raises(ValueError):
        parse_truth(b"XXXX" + d[4:])
    with pytest.raises(ValueError):
        parse_truth(d[:4] + struct.pack("<H", 2) + d[6:])


def test_vel_ned_from_opengl():
    s = sample(1, 0, vx=3.0, vy=2.0, vz=-5.0)  # east 3, up 2, south -5 (= north 5)
    assert s.vel_ned() == (5.0, 3.0, -2.0)


def buffer(**kw):
    kw.setdefault("use_sender_clock", True)
    return TruthBuffer(**kw)


def test_sender_clock_mapping_removes_jitter():
    b = buffer()
    base = 10_000 * MS
    jitter = [0, 3, 1, 7, 0, 2]
    for i, j in enumerate(jitter):
        b.add(sample(i + 1, 1000 + i * 16), arrival_ns=base + (i * 16 + j) * MS)
    ts = [s.t_ns for s in b._ring]
    diffs = {t2 - t1 for t1, t2 in zip(ts, ts[1:])}
    assert diffs == {16 * MS}


def test_ordering_by_sequence():
    b = buffer(use_sender_clock=False)
    assert b.add(sample(1, 0), 100 * MS)
    assert b.add(sample(3, 32), 132 * MS)
    assert not b.add(sample(2, 16), 140 * MS)  # late, out of order: dropped
    assert not b.add(sample(3, 32), 150 * MS)  # duplicate
    assert b.out_of_order == 2


def test_plugin_restart_resets_the_buffer():
    b = buffer()
    for i in range(1, 2001):
        b.add(sample(i, 100_000 + i * 16), (200_000 + i * 16) * MS)
    assert b.add(sample(1, 5), 240_000 * MS)  # seq and sender clock jumped back: restart
    assert len(b) == 1 and b.latest().seq == 1


def test_interpolation_linear_position_and_slerp():
    b = buffer(use_sender_clock=False)
    b.add(sample(1, 0, lat=37.0, elev=100.0, psi=350.0), 1000 * MS)
    b.add(sample(2, 20, lat=37.001, elev=110.0, psi=10.0), 1020 * MS)
    st = b.sample(1010 * MS)
    assert st.latitude == pytest.approx(37.0005)
    assert st.elevation == pytest.approx(105.0)
    assert st.psi_deg == pytest.approx(0.0, abs=1e-6) or st.psi_deg == pytest.approx(360.0, abs=1e-6)
    assert st.extrapolated_s == 0.0


def test_extrapolation_velocity_and_body_rates():
    b = buffer(use_sender_clock=False, max_extrapolation_s=0.1)
    # flying north at 50 m/s (OpenGL south = -50), yawing right at 0.5 rad/s
    b.add(sample(1, 0, lat=37.0, vz=-50.0, r=0.5), 1000 * MS)
    st = b.sample(1040 * MS)
    assert ned_distance(37.0, -122.0, st.latitude, st.longitude) == pytest.approx(2.0, rel=1e-3)
    assert st.latitude > 37.0
    assert math.radians(st.psi_deg) == pytest.approx(0.02, abs=1e-6)
    # beyond max_extrapolation_s the prediction stops (holds)
    st2 = b.sample(1500 * MS)
    assert ned_distance(37.0, -122.0, st2.latitude, st2.longitude) == pytest.approx(5.0, rel=1e-3)
    assert st2.extrapolated_s == pytest.approx(0.1)
    assert st2.stale is False
    st3 = b.sample(2000 * MS)
    assert st3.stale is True


def test_pause_holds_and_replay_freezes():
    b = buffer(use_sender_clock=False)
    b.add(sample(1, 0, lat=37.0, vz=-50.0), 1000 * MS)
    b.add(sample(2, 16, lat=37.0001, vz=-50.0, flags=FLAG_PAUSED), 1016 * MS)
    st = b.sample(1100 * MS)
    assert st.paused and st.latitude == 37.0001 and st.vel_ned == (0.0, 0.0, 0.0)
    b.add(sample(3, 32, lat=36.5, flags=FLAG_REPLAY), 1032 * MS)
    st = b.sample(1100 * MS)
    assert st.replay and st.latitude == 37.0001  # frozen at the last live sample
    b2 = buffer(use_sender_clock=False, replay_policy="follow")
    b2.add(sample(1, 0, lat=36.5, flags=FLAG_REPLAY), 1000 * MS)
    assert b2.sample(1000 * MS).latitude == 36.5


def test_weather_latched():
    b = buffer(use_sender_clock=False)
    b.add(sample(1, 0, weather=Weather(visibility_m=5000.0)), 1000 * MS)
    b.add(sample(2, 16), 1016 * MS)
    assert b.latest_weather.visibility_m == 5000.0


def test_time_dilation_flag():
    b = buffer(use_sender_clock=False)
    s = sample(1, 0)
    s.sim_speed_actual_ogl = 0.8
    b.add(s, 1000 * MS)
    assert b.dilated


# -- xp11_utc ------------------------------------------------------------------


def test_xp11_utc_same_day():
    # local 2026-06-01 10:00 at UTC-8 (lon -122) -> 18:00Z same day
    u = xp11_utc(10 * 3600, 18 * 3600, 6, 1, -122.0, 2026)
    assert u == datetime(2026, 6, 1, 18, 0, tzinfo=UTC)


def test_xp11_utc_local_evening_is_next_utc_day():
    # local 2026-06-01 20:00 at UTC-8 -> 2026-06-02 04:00Z
    u = xp11_utc(20 * 3600, 4 * 3600, 6, 1, -122.0, 2026)
    assert u == datetime(2026, 6, 2, 4, 0, tzinfo=UTC)


def test_xp11_utc_east_of_greenwich_previous_day():
    # local 2026-06-02 03:00 at UTC+9 (Tokyo) -> 2026-06-01 18:00Z
    u = xp11_utc(3 * 3600, 18 * 3600, 6, 2, 139.7, 2026)
    assert u == datetime(2026, 6, 1, 18, 0, tzinfo=UTC)


def test_xp11_utc_new_year_edge_uses_local_year():
    # local 2026-12-31 22:00 at UTC-5 -> 2027-01-01 03:00Z. The local year is 2026.
    u = xp11_utc(22 * 3600, 3 * 3600, 12, 31, -75.0, 2026)
    assert u == datetime(2027, 1, 1, 3, 0, tzinfo=UTC)
    # local 2027-01-01 08:00 at UTC+10 (Sydney-ish) -> 2026-12-31 22:00Z
    u = xp11_utc(8 * 3600, 22 * 3600, 1, 1, 151.0, 2027)
    assert u == datetime(2026, 12, 31, 22, 0, tzinfo=UTC)
