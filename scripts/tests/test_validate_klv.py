"""Unit tests for pure decoding helpers in scripts/validate_klv.py."""
import struct

import validate_klv as vk


def test_crc16_ccitt_known_vector():
    # CRC-16/CCITT-FALSE of "123456789" is 0x29B1 (well-known test vector).
    assert vk.crc16_ccitt(b"123456789") == 0x29B1


def test_crc16_ccitt_empty():
    # With init=0xFFFF and no input bytes, result is the init value.
    assert vk.crc16_ccitt(b"") == 0xFFFF


def test_bcc16_running_sum():
    # Bytes at even index are added to the high byte, odd index to the low.
    # Input: [0x00, 0x01, 0x02, 0x03] → (0<<8) + 1 + (2<<8) + 3 = 0x0204.
    assert vk.bcc16(b"\x00\x01\x02\x03") == 0x0204


def test_bcc16_empty():
    assert vk.bcc16(b"") == 0


def test_decode_ber_short_form():
    # BER short form: length fits in 7 bits. 0x05 → length 5, 1 byte consumed.
    length, consumed = vk.decode_ber_length(b"\x05rest", 0)
    assert length == 5
    assert consumed == 1


def test_decode_ber_long_form():
    # Long form: 0x82 signals 2 length-bytes to follow; 0x01 0x00 → 256.
    length, consumed = vk.decode_ber_length(b"\x82\x01\x00", 0)
    assert length == 256
    assert consumed == 3


def test_parse_klv_local_set_stores_tag_by_int_key():
    # Build a minimal ST 0601 Local Set containing tag 65 (UAS LS version = 9),
    # followed by a tag 1 checksum TLV. The parser stores tags directly under
    # their integer key in the result dict.
    version_tlv = bytes([65, 1, 9])
    checksum_tlv = bytes([1, 2, 0, 0])  # bogus checksum; parser still records the tag
    body = version_tlv + checksum_tlv
    packet = vk.ST0601_UL + bytes([len(body)]) + body

    result = vk.parse_klv_local_set(packet)
    assert isinstance(result, dict)
    assert 65 in result
    assert result["_raw_size"] == len(packet)


def test_parse_klv_local_set_rejects_bad_universal_label():
    bad_ul = bytes(16)  # all zeros — not the ST 0601 key
    packet = bad_ul + bytes([0])  # zero-length body
    result = vk.parse_klv_local_set(packet)
    assert any("UL mismatch" in e for e in result["_errors"])


def test_decode_lat_lon_symmetry():
    # Lat/lon use signed int32 with full-range mapping; +max and -max should
    # bracket ±range_deg within a tiny float tolerance.
    plus_max = struct.pack(">i", 0x7FFFFFFF)
    minus_max = struct.pack(">i", -0x7FFFFFFF)
    assert "90.000000" in vk.decode_lat_lon(plus_max, 90.0)
    assert "-90.000000" in vk.decode_lat_lon(minus_max, 90.0)
