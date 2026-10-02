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
    # Byte 4: Minor Version 3 (bits 4-7) | Timestamp Valid | IG Mode Operate.
    assert pkt[4] == 0x35
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
    # Byte 5 enables (CigiViewDefV3::Pack): near 0x01 | far 0x02 | left 0x04 |
    # right 0x08 | top 0x10 | bottom 0x20.
    assert pkt[5] == 0x3F
    assert pkt[6] == 0  # perspective, no reorder, view type 0
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


def test_pack_art_part_control_layout():
    pkt = sc.pack_art_part_control(1, 0, pitch=-30.0, yaw=45.0)
    assert len(pkt) == 32 and pkt[0] == 6 and pkt[1] == 32
    assert struct.unpack(">H", pkt[2:4])[0] == 1
    assert pkt[4] == 0 and pkt[5] == 0x71
    roll, pitch, yaw = struct.unpack(">fff", pkt[20:32])
    assert (roll, pitch, yaw) == (0.0, -30.0, 45.0)


def test_pack_view_control_layout_matches_ccl():
    # CigiViewCtrlV3::Pack: 2-3 View ID, 4 Group ID, 5 enables, 6-7 Entity ID,
    # 8-19 X/Y/Z offsets, 20 Roll, 24 Pitch, 28 Yaw.
    pkt = sc.pack_view_control(90.0, -45.0, 5.0, view_id=0x0102, group_id=3, entity_id=0x0405)
    assert len(pkt) == 32 and pkt[0] == 16 and pkt[1] == 32
    assert struct.unpack(">H", pkt[2:4])[0] == 0x0102
    assert pkt[4] == 3
    assert pkt[5] == 0x38  # roll | pitch | yaw enable, offsets off
    assert struct.unpack(">H", pkt[6:8])[0] == 0x0405
    assert struct.unpack(">ffffff", pkt[8:32]) == (0.0, 0.0, 0.0, 5.0, -45.0, 90.0)


def test_pack_view_control_wraps_yaw_to_pm180():
    pkt = sc.pack_view_control(270.0, -10.0)
    assert struct.unpack(">f", pkt[28:32])[0] == -90.0
    assert struct.unpack(">f", sc.pack_view_control(180.0, 0.0)[28:32])[0] == -180.0


def test_build_host_frame_with_gimbal():
    d = sc.build_host_frame(0, 1, 37.0, -122.0, 100.0, 0.0, 0.0, 0.0, include_view_def=False, gimbal=(90.0, -45.0, 0.0))
    assert _ids(d) == [1, 2, 17, 16]
    assert 16 not in _ids(sc.build_host_frame(0, 1, 37.0, -122.0, 100.0, 0.0, 0.0, 0.0))


def _ids(d):
    ids, i = [], 0
    while i < len(d):
        ids.append(d[i])
        i += d[i + 1]
    return ids
