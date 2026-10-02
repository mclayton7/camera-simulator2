"""CIGI packers against the CCL V3 Pack() byte offsets, packet 201 against
hitl/PROTOCOL.md, and (when the CCL sources are built) a decode by CCL itself."""

import math
import shutil
import struct
import subprocess
from pathlib import Path

import pytest

from camsim_hitl import cigi

REPO = Path(__file__).resolve().parents[2]


def test_ig_control_layout():
    p = cigi.pack_ig_control(0x01020304, 0xA0B0C0D0, last_ig_frame=77, db_number=-2)
    assert len(p) == 24
    assert p[0] == 1 and p[1] == 24 and p[2] == 3
    assert struct.unpack_from(">b", p, 3)[0] == -2
    assert p[4] == 0x35  # minor 3 << 4 | timestamp valid | operate
    assert p[5] == 0
    assert p[6:8] == b"\x80\x00"  # Byte Swap Magic, big-endian
    assert struct.unpack_from(">III", p, 8) == (0x01020304, 0xA0B0C0D0, 77)
    assert p[20:24] == b"\0\0\0\0"


def test_entity_control_layout():
    p = cigi.pack_entity_control(1, 37.5, -122.25, 123.5, 350.0, -5.0, 10.0)
    assert len(p) == 48 and p[0] == 2 and p[1] == 48
    assert struct.unpack_from(">H", p, 2)[0] == 1
    assert p[4] == 0x01  # Active, Detach
    assert p[6] == 255
    assert struct.unpack_from(">HH", p, 8) == (0, 0)
    assert struct.unpack_from(">fff", p, 12) == (10.0, -5.0, 350.0)  # roll, pitch, yaw
    assert struct.unpack_from(">ddd", p, 24) == (37.5, -122.25, 123.5)


def test_view_control_layout_matches_ccl():
    # CigiViewCtrlV3::Pack: 2-3 View ID, 4 Group, 5 flags, 6-7 Entity ID, 8.. floats
    p = cigi.pack_view_control(-90.0, -45.0, 3.0, view_id=0x0102, group_id=5, entity_id=0x0304)
    assert len(p) == 32 and p[0] == 16 and p[1] == 32
    assert struct.unpack_from(">H", p, 2)[0] == 0x0102
    assert p[4] == 5
    assert p[5] == 0x38  # roll 0x08 | pitch 0x10 | yaw 0x20, no offsets
    assert struct.unpack_from(">H", p, 6)[0] == 0x0304
    assert struct.unpack_from(">ffffff", p, 8) == (0.0, 0.0, 0.0, 3.0, -45.0, -90.0)


def test_view_definition_layout():
    p = cigi.pack_view_definition(40.0, 22.5)
    assert len(p) == 32 and p[0] == 21 and p[1] == 32
    assert p[5] == 0x3F and p[6] == 0  # all six enables; perspective
    near, far, left, right, top, bottom = struct.unpack_from(">ffffff", p, 8)
    assert (left, right, top, bottom) == (-20.0, 20.0, 11.25, -11.25)
    assert cigi.vfov_from_hfov(90.0, 16, 9) == pytest.approx(2 * math.degrees(math.atan(9 / 16)))


def test_sensor_control_layout():
    p = cigi.pack_sensor_control(1, polarity=1)
    assert len(p) == 24 and p[0] == 17
    assert struct.unpack_from(">H", p, 2)[0] == 0
    assert p[4] == 1 and p[5] == 0x03  # on | black-hot


def test_celestial_layout():
    p = cigi.pack_celestial_control(13, 37, 12, 31, 2026, ephemeris=True, date_valid=True)
    assert len(p) == 16 and p[0] == 9 and p[1] == 16
    assert p[2] == 13 and p[3] == 37
    assert p[4] == 0x1F
    assert struct.unpack_from(">I", p, 8)[0] == 12312026
    p = cigi.pack_celestial_control(ephemeris=False, date_valid=False)
    assert p[4] == 0x0E


def test_atmosphere_layout():
    p = cigi.pack_atmosphere_control(5000.0, 15.0, 60.4, 5.0, 270.0, 1013.0)
    assert len(p) == 32 and p[0] == 10 and p[2] == 1 and p[3] == 60
    assert struct.unpack_from(">ffffff", p, 4) == (15.0, 5000.0, 5.0, 0.0, 270.0, 1013.0)


def test_weather_layout_matches_camsim_raw_parser():
    p = cigi.pack_weather_control(75.0, 1480.0, 400.0, 9000.0, cloud_type=9, wind_dir_deg=270.0)
    assert len(p) == 56 and p[0] == 12 and p[1] == 56
    assert struct.unpack_from(">H", p, 2)[0] == 0 and p[4] == 1
    assert p[6] == (0x01 | (9 << 4)) and p[7] == 0  # enable | cloud type; global scope
    assert struct.unpack_from(">f", p, 12)[0] == 9000.0
    assert struct.unpack_from(">fff", p, 20) == (75.0, 1480.0, 400.0)
    assert struct.unpack_from(">f", p, 44)[0] == -90.0  # CCL folds >180 to negative


def test_wave_and_hat_hot_match_ocean_check_layouts():
    p = cigi.pack_wave_control(1.5, 45.0, 5.4, 60.0)
    assert len(p) == 32 and p[0] == 14 and p[5] == 1
    assert struct.unpack_from(">ffff", p, 8) == pytest.approx((1.5, 45.0, 5.4, 60.0))
    p = cigi.pack_hat_hot_request(9, 37.8, -122.4, 50.0)
    assert len(p) == 32 and p[0] == 24
    assert struct.unpack_from(">H", p, 2)[0] == 9
    assert p[4] == 2 and p[5] == 0
    assert struct.unpack_from(">ddd", p, 8) == (37.8, -122.4, 50.0)


def test_platform_kinematics_matches_protocol_md():
    p = cigi.pack_platform_kinematics(
        1, true_airspeed=31.5, indicated_airspeed=28.25, magnetic_heading=-10.0,
        vel_ned=(1.5, -2.5, 0.25), sample_utc=1790000000.125,
    )
    assert len(p) == 48
    assert p[0] == 201 and p[1] == 48
    assert struct.unpack_from(">H", p, 2)[0] == 1
    assert p[4] == 0x0F and p[5:8] == b"\0\0\0"
    assert struct.unpack_from(">ffffff", p, 8) == (31.5, 28.25, 350.0, 1.5, -2.5, 0.25)
    assert struct.unpack_from(">d", p, 32)[0] == 1790000000.125
    assert p[40:48] == b"\0" * 8
    q = cigi.pack_platform_kinematics(1, vel_ned=(1.0, 2.0, 3.0))
    assert q[4] == cigi.PK_NED_VELOCITY
    assert struct.unpack_from(">fff", q, 8) == (0.0, 0.0, 0.0)


@pytest.mark.parametrize("order", ["<", ">"])
def test_response_parsing_both_byte_orders(order):
    data = (
        cigi.pack_sof(1234, last_host_frame=99, order=order)
        + cigi.pack_hat_hot_response(3, True, 12.5, is_hot=True, order=order)
        + cigi.pack_hat_hot_ext_response(4, True, 88.0, 12.25, order=order)
        + cigi.pack_sensor_ext_response(1233, 37.1, -122.2, -20.5, sensor_id=1, order=order)
    )
    m = cigi.parse_response_message(data)
    assert m.big_endian == (order == ">")
    assert m.opcodes == [101, 102, 103, 107]
    assert m.sof.ig_frame == 1234 and m.sof.last_host_frame == 99 and m.sof.ig_mode == 1
    a, b = m.hat_hot
    assert (a.request_id, a.valid, a.hot, a.hat, a.extended) == (3, True, 12.5, None, False)
    assert (b.request_id, b.hot, b.hat, b.extended) == (4, 12.25, 88.0, True)
    s = m.sensor_ext[0]
    assert (s.frame, s.lat, s.lon, s.alt, s.sensor_id, s.sensor_status) == (1233, 37.1, -122.2, -20.5, 1, 1)


def test_sof_layout_little_endian_like_camsim():
    p = cigi.pack_sof(5, order="<")
    assert p[6:8] == b"\x00\x80"  # 0x8000 in the IG's native (little-endian) order
    assert len(p) == 24


def test_invalid_hat_hot():
    m = cigi.parse_response_message(
        cigi.pack_sof(1) + cigi.pack_hat_hot_ext_response(1, False, 0.0, 0.0)
    )
    assert not m.hat_hot[0].valid and m.hat_hot[0].hot is None


# -- decode by CCL itself (what CamSim runs) -----------------------------------


@pytest.fixture(scope="session")
def ccl_harness():
    src = REPO / ".build_tmp/ccl/source"
    inc = REPO / ".build_tmp/ccl/include"
    gxx = shutil.which("g++")
    if not src.is_dir() or gxx is None:
        pytest.skip("CCL sources (.build_tmp/ccl, scripts/build_thirdparty.sh) or g++ not available")
    out = REPO / ".build_tmp/ccl_harness/ccl_harness"
    here = Path(__file__).with_name("ccl_harness.cpp")
    if not out.exists() or out.stat().st_mtime < here.stat().st_mtime:
        out.parent.mkdir(parents=True, exist_ok=True)
        cmd = [gxx, "-O1", "-w", "-std=c++17", f"-I{inc}", f"-I{src}", str(here), *map(str, sorted(src.glob("*.cpp"))), "-o", str(out)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            pytest.skip(f"CCL harness did not build: {r.stderr[-500:]}")
    return out


def ccl_decode(harness, data: bytes) -> dict:
    r = subprocess.run([str(harness), data.hex()], capture_output=True, text=True, check=True)
    return dict(line.split("=", 1) for line in r.stdout.splitlines() if "=" in line)


def test_ccl_decodes_our_packets(ccl_harness):
    d = (
        cigi.pack_ig_control(5, 123456)
        + cigi.pack_entity_control(1, 37.5, -122.25, 123.5, 350.0, -5.0, 10.0)
        + cigi.pack_platform_kinematics(1, 30.0, 28.0, 12.0, (1.0, 2.0, 3.0), 1.7e9)
        + cigi.pack_view_control(-90.0, -45.0, 3.0)
        + cigi.pack_view_definition(40.0, 22.5)
        + cigi.pack_sensor_control(1, 1)
        + cigi.pack_hat_hot_request(7, 37.5, -122.25, 100.0)
        + cigi.pack_wave_control(1.0, 30.0, 4.4, 90.0)
    )
    f = ccl_decode(ccl_harness, d)
    assert f["status"] == "0"
    assert f["ig.minor"] == "3" and f["ig.frame"] == "5" and f["ig.timestamp"] == "123456" and f["ig.ts_valid"] == "1"
    assert f["ent.id"] == "1" and f["ent.state"] == "1" and f["ent.attach"] == "0"
    assert float(f["ent.lat"]) == 37.5 and float(f["ent.lon"]) == -122.25 and float(f["ent.alt"]) == 123.5
    assert (float(f["ent.yaw"]), float(f["ent.pitch"]), float(f["ent.roll"])) == (350.0, -5.0, 10.0)
    # View Control: a negative yaw survives CCL's unpack (only its packer rejects it)
    assert f["vc.entity"] == "0" and f["vc.yaw_en"] == f["vc.pitch_en"] == f["vc.roll_en"] == "1" and f["vc.x_en"] == "0"
    assert (float(f["vc.yaw"]), float(f["vc.pitch"]), float(f["vc.roll"])) == (-90.0, -45.0, 3.0)
    assert (float(f["vd.left"]), float(f["vd.right"])) == (-20.0, 20.0)
    assert f["sc.sensor"] == "1" and f["sc.on"] == "1" and f["sc.polarity"] == "1"
    assert f["hh.id"] == "7" and f["hh.type"] == "2" and f["hh.coord"] == "0"
    assert float(f["wv.height"]) == 1.0 and float(f["wv.dir"]) == 90.0
    assert "other" not in f  # the user packet 201 was skipped by size, nothing after it lost
