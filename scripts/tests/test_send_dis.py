"""Unit tests for scripts/send_dis_test.py (PDU layout mirrors DIS/DisPduTypes.cpp)."""

import math
import struct

import send_dis_test as sd


def _pdu(**kw):
    args = {"entity_id": 3, "entity_type": sd.TRUCK_TYPE, "lat": 37.795, "lon": -122.46, "alt": 0.0,
            "heading_deg": 90.0, "speed_mps": 15.0, "yaw_rate_dps": 2.0, "exercise": 1, "marking": "TRUCK1", "t": 12.5}
    args.update(kw)
    return sd.pack_entity_state(**args)


def test_header_and_length():
    p = _pdu()
    assert len(p) == 144
    assert p[0] == 7 and p[1] == 1 and p[2] == 1 and p[3] == 1  # version, exercise, ESPDU, family
    assert struct.unpack(">H", p[8:10])[0] == 144


def test_entity_id_type_and_dr():
    p = _pdu()
    assert struct.unpack(">HHH", p[12:18]) == (1, 1, 3)
    assert p[19] == 0  # no articulation parameters
    kind, domain, country, cat, sub, spec, extra = struct.unpack(">BBHBBBB", p[20:28])
    assert (kind, domain, country, cat, sub, spec, extra) == sd.TRUCK_TYPE
    assert p[88] == 4  # DR algorithm 4
    assert p[129:135] == b"TRUCK1"


def test_location_is_ecef_of_lat_lon():
    p = _pdu()
    x, y, z = struct.unpack(">ddd", p[48:72])
    ex, ey, ez = sd.geodetic_to_ecef(37.795, -122.46, 0.0)
    assert (x, y, z) == (ex, ey, ez)
    assert abs(math.sqrt(x * x + y * y + z * z) - 6_370_000) < 20_000


def test_velocity_is_ecef_heading_east_at_equator():
    p = sd.pack_entity_state(entity_id=1, entity_type=sd.BOAT_TYPE, lat=0.0, lon=0.0, alt=0.0,
                             heading_deg=90.0, speed_mps=10.0, yaw_rate_dps=0.0)
    vx, vy, vz = struct.unpack(">fff", p[36:48])
    # At (0, 0) East is ECEF +Y.
    assert abs(vx) < 1e-5 and abs(vy - 10.0) < 1e-5 and abs(vz) < 1e-5


def test_euler_heading_east_at_equator():
    # BodyToEcef = NedToEcef(0,0) * Rz(90 deg) -> psi = +90 deg, theta = 0, phi = -90 deg.
    psi, theta, phi = sd.heading_to_dis_euler(90.0, 0.0, 0.0)
    assert abs(psi - math.pi / 2) < 1e-9 and abs(theta) < 1e-9 and abs(phi + math.pi / 2) < 1e-9


def test_angular_velocity_is_yaw_rate_in_body_z():
    p = _pdu(yaw_rate_dps=6.0)
    wx, wy, wz = struct.unpack(">fff", p[116:128])
    assert wx == 0.0 and wy == 0.0 and abs(wz - math.radians(6.0)) < 1e-6


def test_path_follower_stays_on_circle():
    boat = sd.PRESETS["boat-circle"]
    f = sd.PathFollower(boat.waypoints_ne, boat.speed_mps)
    for t in range(0, 300, 7):
        n, e, _, _ = f.state(float(t))
        assert abs(math.hypot(n, e) - boat.radius_m) < 5.0


def test_yaw_rate_wraps():
    # The boat circle's heading crosses 0/360 every lap; the yaw rate must stay the real
    # turn rate v/r (~3.06 deg/s), never a +-360 deg jump divided by the time step.
    boat = sd.PRESETS["boat-circle"]
    f = sd.PathFollower(boat.waypoints_ne, boat.speed_mps)
    expected = abs(boat.speed_mps / boat.radius_m * 180.0 / math.pi)
    headings, rates = [], []
    for t in range(1300):  # 130 s > one lap (~118 s)
        _, _, h, r = f.state(t / 10.0)
        headings.append(h)
        rates.append(abs(r))
    assert min(headings) < 10.0 and max(headings) > 350.0  # the wrap happened
    assert max(rates) < expected * 1.5
