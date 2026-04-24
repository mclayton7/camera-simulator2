"""Unit tests for pure packet builders in scripts/send_cigi_test.py."""
import struct

import send_cigi_test as sc


def test_pack_ig_control_header():
    pkt = sc.pack_ig_control(frame_ctr=0x12345678, db_number=0)
    assert len(pkt) == 24
    # Byte 0: packet ID = 1, Byte 1: size = 24, Byte 2: major = 3.
    assert pkt[0] == 1
    assert pkt[1] == 24
    assert pkt[2] == 3
    # Byte 3: database number (signed int8, zero here).
    assert pkt[3] == 0
    # Byte 4: IG mode = Operate + Timestamp-valid flag (0x05).
    assert pkt[4] == 0x05
    # Bytes 6-7: byte swap magic (big-endian 0x8000).
    assert struct.unpack(">H", pkt[6:8])[0] == 0x8000
    # Bytes 8-11: host frame counter (big-endian).
    assert struct.unpack(">I", pkt[8:12])[0] == 0x12345678


def test_pack_entity_control_size_and_fields():
    pkt = sc.pack_entity_control(
        entity_id=42,
        lat=37.7749,
        lon=-122.4194,
        alt=1000.0,
        yaw=90.0,
        pitch=-15.0,
        roll=0.5,
    )
    assert len(pkt) == 48
    assert pkt[0] == 2  # packet ID
    assert pkt[1] == 48  # size
    assert struct.unpack(">H", pkt[2:4])[0] == 42
    # Angles at bytes 12-23 (3× float32 big-endian: roll, pitch, yaw).
    roll, pitch, yaw = struct.unpack(">fff", pkt[12:24])
    assert abs(roll - 0.5) < 1e-5
    assert abs(pitch - -15.0) < 1e-3
    assert abs(yaw - 90.0) < 1e-3
    # Position at bytes 24-47 (3× float64 big-endian: lat, lon, alt).
    lat, lon, alt = struct.unpack(">ddd", pkt[24:48])
    assert abs(lat - 37.7749) < 1e-9
    assert abs(lon - -122.4194) < 1e-9
    assert abs(alt - 1000.0) < 1e-9


def test_pack_view_definition_size_and_flags():
    pkt = sc.pack_view_definition(view_id=0, fov_left=-30.0, fov_right=30.0)
    assert len(pkt) == 32
    assert pkt[0] == 21  # packet ID (View Definition)
    assert pkt[1] == 32  # size
    # Byte 5 flags: all five enable bits set (bits 0-4 → 0x1F).
    assert pkt[5] == 0b00011111
    # FOV left/right at bytes 16-23 (float32 big-endian).
    fov_left, fov_right = struct.unpack(">ff", pkt[16:24])
    assert abs(fov_left - -30.0) < 1e-5
    assert abs(fov_right - 30.0) < 1e-5


def test_orbit_position_returns_offset_and_inward_yaw():
    # Heading 90° (east) at 1 km radius should offset longitude eastward and
    # leave the returned yaw pointing back toward the centre (heading + 180).
    center_lat, center_lon = 37.7749, -122.4194
    lat, lon, alt, yaw = sc.orbit_position(
        center_lat=center_lat,
        center_lon=center_lon,
        alt=100.0,
        radius_m=1000.0,
        heading_deg=90.0,
    )
    assert alt == 100.0
    # 1 km at this latitude shifts longitude by roughly 0.011°; keep the
    # tolerance loose so small great-circle vs. local-flat deltas don't fail.
    assert 0.005 < (lon - center_lon) < 0.02
    assert abs(lat - center_lat) < 1e-3  # due east: latitude ~unchanged
    # Yaw back toward centre: heading + 180 = 270° (west).
    assert abs(yaw - 270.0) < 1e-9
