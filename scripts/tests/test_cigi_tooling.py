"""Gap 16 fixes: cigi_web_ui's View Control packer and check_cigi_responses' 103/107 parsing."""

import struct

import check_cigi_responses as ccr
import cigi_web_ui as ui


def test_web_ui_view_control_layout_matches_ccl():
    pkt = ui.pack_view_control(view_id=0x0102, entity_id=0x0304, roll=1.0, pitch=-45.0, yaw=-90.0, group_id=7)
    assert len(pkt) == 32 and pkt[0] == 16 and pkt[1] == 32
    assert struct.unpack(">H", pkt[2:4])[0] == 0x0102  # View ID
    assert pkt[4] == 7  # Group ID
    assert pkt[5] == 0x3F  # X/Y/Z offset + roll/pitch/yaw enables
    assert struct.unpack(">H", pkt[6:8])[0] == 0x0304  # Entity ID
    assert struct.unpack(">ffffff", pkt[8:32]) == (0.0, 0.0, 0.0, 1.0, -45.0, -90.0)


def _sof(order, frame):
    # CamSim's CCL SOF (V3_2): 24 bytes, Byte Swap Magic 0x8000 in its native order
    return struct.pack(order + "BBBbBBHIIII", 101, 24, 3, 0, 0, 0x25, 0x8000, frame, 0, 0, 0)


def _datagram(order):
    return (
        _sof(order, 42)
        + struct.pack(order + "BBHBBHd", 102, 16, 5, 0x03, 0, 0, 12.5)  # valid HOT
        + struct.pack(order + "BBHBBHddIffI", 103, 40, 6, 0x01, 0, 0, 88.0, -20.25, 0, 0.0, 90.0, 0)
        + struct.pack(order + "BBHBBHHHffIddd", 107, 48, 0, 1, 1, 0, 0, 0, 0.0, 0.0, 41, 37.5, -122.25, -30.5)
    )


def test_parse_responses_little_and_big_endian():
    for order in ("<", ">"):
        r = ccr.parse_responses(_datagram(order))
        assert r["ids"] == [101, 102, 103, 107]
        assert r["sof_frame"] == 42
        assert r["hat_hot"][0] == {"op": 102, "id": 5, "valid": True, "hot": 12.5}
        assert r["hat_hot"][1] == {"op": 103, "id": 6, "valid": True, "hat": 88.0, "hot": -20.25}
        c = r["sensor_ext"][0]
        assert (c["frame"], c["lat"], c["lon"], c["alt"], c["sensor_id"], c["status"]) == (41, 37.5, -122.25, -30.5, 1, 1)


def test_parse_packet_ids_still_counts_everything():
    assert ccr.parse_packet_ids(_datagram("<")) == [101, 102, 103, 107]
    assert ccr.byte_order(_sof("<", 1)) == "<" and ccr.byte_order(_sof(">", 1)) == ">"
