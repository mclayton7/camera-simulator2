#!/usr/bin/env python3
"""
validate_klv.py — MISB ST 0601 KLV metadata validator for CamSim.

Reads an MPEG-TS stream from UDP (or a .ts file), finds the KLVA data PID,
reassembles PES packets, and decodes MISB ST 0601 Local Set KLV. Validates:
  • Universal Label key
  • BER length
  • Tag-value decoding for common ST 0601 tags
  • CRC-16/CCITT checksum (Tag 1)

Usage:
    # Live UDP multicast (Phase 4 test):
    python3 scripts/validate_klv.py --addr 239.1.1.1 --port 5004

    # From a saved .ts file:
    python3 scripts/validate_klv.py --file /tmp/camsim_capture.ts

    # Save a capture first (via test_video_output.sh --save):
    ./scripts/test_video_output.sh --save /tmp/cap.ts --duration 5
    python3 scripts/validate_klv.py --file /tmp/cap.ts

Options:
    --addr ADDR       Multicast/unicast address  (default: 239.1.1.1)
    --port PORT       UDP port                   (default: 5004)
    --file FILE       Read from .ts file instead of UDP
    --count N         Stop after decoding N KLV packets (default: 5)
    --verbose         Print raw hex of each KLV packet
"""

import argparse
import socket
import struct
import sys
import time
from typing import Iterator

# ---------------------------------------------------------------------------
# MISB ST 0601 Universal Label (16 bytes)
# ---------------------------------------------------------------------------
ST0601_UL = bytes([
    0x06, 0x0E, 0x2B, 0x34,
    0x02, 0x0B, 0x01, 0x01,
    0x0E, 0x01, 0x03, 0x01,
    0x01, 0x00, 0x00, 0x00,
])

# ---------------------------------------------------------------------------
# CRC-16/CCITT (poly=0x1021, init=0xFFFF) — matches FKlvBuilder::ComputeCrc16
# ---------------------------------------------------------------------------

def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


# ---------------------------------------------------------------------------
# MISB ST 0601 tag decoders
# ---------------------------------------------------------------------------

def decode_timestamp(v: bytes) -> str:
    ts = int.from_bytes(v, "big")
    return f"{ts} μs ({time.strftime('%H:%M:%S', time.gmtime(ts / 1e6))}.{ts % 1_000_000:06d} UTC)"


def decode_lat_lon(v: bytes, range_deg: float) -> str:
    raw = int.from_bytes(v, "big", signed=True)
    deg = raw * range_deg / 0x7FFF_FFFF
    return f"{deg:.6f}°"


def decode_altitude(v: bytes) -> str:
    raw = int.from_bytes(v, "big")
    metres = raw * (19000.0 + 900.0) / 65535.0 - 900.0
    return f"{metres:.1f} m"


def decode_fov(v: bytes) -> str:
    raw = int.from_bytes(v, "big")
    deg = raw * 180.0 / 65535.0
    return f"{deg:.3f}°"


def decode_angle360(v: bytes) -> str:
    raw = int.from_bytes(v, "big", signed=True)
    deg = raw * 360.0 / 0x7FFF_FFFF
    return f"{deg:.3f}°"


TAG_DECODERS = {
    1:  ("Checksum",           lambda v: f"0x{int.from_bytes(v,'big'):04X}"),
    2:  ("UNIX Timestamp",     decode_timestamp),
    13: ("Sensor Latitude",    lambda v: decode_lat_lon(v,  90.0)),
    14: ("Sensor Longitude",   lambda v: decode_lat_lon(v, 180.0)),
    15: ("Sensor Altitude",    decode_altitude),
    18: ("Sensor HFOV",        decode_fov),
    19: ("Sensor VFOV",        decode_fov),
    26: ("Sensor Azimuth",     decode_angle360),
    27: ("Sensor Elevation",   decode_angle360),
    28: ("Sensor Roll",        decode_angle360),
}


# ---------------------------------------------------------------------------
# BER-OID length decoder
# ---------------------------------------------------------------------------

def decode_ber_length(data: bytes, offset: int) -> tuple[int, int]:
    """Returns (length_value, bytes_consumed)."""
    b0 = data[offset]
    if b0 < 0x80:
        return b0, 1
    n = b0 & 0x7F
    if n == 0 or offset + n >= len(data):
        raise ValueError(f"Invalid BER length at offset {offset}")
    length = int.from_bytes(data[offset + 1: offset + 1 + n], "big")
    return length, 1 + n


# ---------------------------------------------------------------------------
# KLV Local Set parser
# ---------------------------------------------------------------------------

def parse_klv_local_set(packet: bytes, verbose: bool = False) -> dict:
    """
    Parse a MISB ST 0601 Local Set KLV packet.
    Returns a dict of {tag: (name, decoded_value)} plus '_errors' list.
    """
    result: dict = {"_errors": [], "_raw_size": len(packet)}

    if verbose:
        hex_str = " ".join(f"{b:02X}" for b in packet)
        print(f"  Raw ({len(packet)} bytes): {hex_str}")

    # Validate Universal Label
    if len(packet) < 16:
        result["_errors"].append("Packet too short for UL key")
        return result

    ul = packet[:16]
    if ul != ST0601_UL:
        result["_errors"].append(
            f"UL mismatch: got {ul.hex()} expected {ST0601_UL.hex()}"
        )
        return result

    # Decode BER length
    try:
        value_len, ber_bytes = decode_ber_length(packet, 16)
    except ValueError as e:
        result["_errors"].append(str(e))
        return result

    header_len = 16 + ber_bytes
    expected_total = header_len + value_len
    if len(packet) < expected_total:
        result["_errors"].append(
            f"Packet truncated: got {len(packet)}, expected {expected_total}"
        )
        return result

    # Parse TLV triplets
    pos = header_len
    end = header_len + value_len
    crc_offset = None  # byte offset of Tag 1 value in packet

    while pos < end:
        if pos >= len(packet):
            break
        tag = packet[pos]; pos += 1
        if pos >= len(packet):
            result["_errors"].append(f"Truncated at tag {tag} length byte")
            break
        length = packet[pos]; pos += 1  # ST 0601 uses 1-byte lengths only
        if pos + length > len(packet):
            result["_errors"].append(f"Tag {tag}: length {length} overruns packet")
            break

        value = packet[pos: pos + length]

        if tag == 1:
            # CRC is over the entire packet up to (not including) the CRC value
            # i.e. from byte 0 through the tag and length bytes of tag 1
            crc_offset = pos
        else:
            if tag in TAG_DECODERS:
                name, decoder = TAG_DECODERS[tag]
                try:
                    decoded = decoder(value)
                    result[tag] = (name, decoded)
                except Exception as e:
                    result[tag] = (f"Tag {tag}", f"[decode error: {e}]")
            else:
                result[tag] = (f"Tag {tag} (unknown)", value.hex())

        pos += length

    # CRC validation
    if crc_offset is not None:
        # CRC covers: UL key + BER length + all TLVs up to (not including) Tag1 value
        # = packet[0 .. crc_offset-2]   (exclude tag=1, length=2 bytes too)
        data_for_crc = packet[:crc_offset - 2]   # exclude "01 02" tag+length of CRC tag
        expected_crc = crc16_ccitt(data_for_crc)
        actual_crc   = int.from_bytes(packet[crc_offset: crc_offset + 2], "big")
        if expected_crc == actual_crc:
            result["_crc"] = f"OK (0x{actual_crc:04X})"
        else:
            result["_crc"] = f"FAIL (got 0x{actual_crc:04X}, expected 0x{expected_crc:04X})"
            result["_errors"].append("CRC mismatch")
    else:
        result["_crc"] = "Tag 1 not found"

    return result


def print_klv_result(r: dict, frame_num: int) -> None:
    errors = r.get("_errors", [])
    crc    = r.get("_crc", "")
    size   = r.get("_raw_size", 0)

    status = "OK" if not errors else "ERRORS"
    print(f"\n{'='*60}")
    print(f"KLV Packet #{frame_num}  ({size} bytes)  [{status}]")
    print(f"{'='*60}")

    for tag in sorted(k for k in r if isinstance(k, int) and k != 1):
        name, val = r[tag]
        print(f"  Tag {tag:3d}  {name:<30s}  {val}")

    print(f"  Tag   1  {'Checksum':<30s}  {crc}")

    if errors:
        for e in errors:
            print(f"  [ERROR] {e}")


# ---------------------------------------------------------------------------
# MPEG-TS parsing
# ---------------------------------------------------------------------------

TS_PACKET_SIZE = 188
TS_SYNC_BYTE   = 0x47

def iter_ts_packets(data: bytes) -> Iterator[bytes]:
    """Yield 188-byte TS packets from a buffer."""
    i = 0
    while i + TS_PACKET_SIZE <= len(data):
        if data[i] == TS_SYNC_BYTE:
            yield data[i: i + TS_PACKET_SIZE]
            i += TS_PACKET_SIZE
        else:
            i += 1  # resync


def parse_ts_header(pkt: bytes) -> dict:
    h = struct.unpack(">I", pkt[:4])[0]
    return {
        "sync":              (h >> 24) & 0xFF,
        "tei":               (h >> 23) & 0x1,
        "pusi":              (h >> 22) & 0x1,   # payload unit start indicator
        "pid":               (h >> 8)  & 0x1FFF,
        "adaptation_field":  (h >> 5)  & 0x3,
        "continuity_ctr":     h        & 0xF,
    }


def extract_pes_payload(pkt: bytes) -> bytes:
    """Return the payload bytes from a TS packet (after adaptation field if any)."""
    hdr = parse_ts_header(pkt)
    af  = hdr["adaptation_field"]
    offset = 4
    if af in (2, 3):  # has adaptation field
        af_len = pkt[4]
        offset = 5 + af_len
    return pkt[offset:] if offset < len(pkt) else b""


def find_klv_pid_from_pat_pmt(ts_buffer: bytes) -> int | None:
    """
    Walk PAT → PMT to find the PID of a stream with codec_tag KLVA or
    stream_type 0x15 (metadata), or 0x06 (private data used by some muxers).
    Returns the PID or None if not found.
    """
    pat_pmt_pid = None

    for pkt in iter_ts_packets(ts_buffer):
        hdr = parse_ts_header(pkt)
        pid = hdr["pid"]

        if pid == 0:  # PAT
            payload = extract_pes_payload(pkt)
            if len(payload) < 8:
                continue
            # Skip table_id, section_length etc.  PAT entries start at byte 8
            # (after pointer_field at offset 0 if PUSI is set)
            ptr = payload[0] if hdr["pusi"] else 0
            base = 1 + ptr
            # Parse PAT section: skip 8-byte header
            if len(payload) < base + 8:
                continue
            entry_start = base + 8
            while entry_start + 4 <= len(payload) - 4:  # -4 for CRC
                prog_num = struct.unpack(">H", payload[entry_start: entry_start + 2])[0]
                pmt_pid  = struct.unpack(">H", payload[entry_start + 2: entry_start + 4])[0] & 0x1FFF
                if prog_num != 0:
                    pat_pmt_pid = pmt_pid
                    break
                entry_start += 4
            if pat_pmt_pid:
                break

    if pat_pmt_pid is None:
        return None

    # Scan PMT
    for pkt in iter_ts_packets(ts_buffer):
        hdr = parse_ts_header(pkt)
        if hdr["pid"] != pat_pmt_pid:
            continue

        payload = extract_pes_payload(pkt)
        ptr = payload[0] if hdr["pusi"] else 0
        base = 1 + ptr
        if len(payload) < base + 12:
            continue

        # section_length at base+1 (10 bits)
        section_len = struct.unpack(">H", payload[base + 1: base + 3])[0] & 0x0FFF
        prog_info_len = struct.unpack(">H", payload[base + 10: base + 12])[0] & 0x0FFF
        es_start = base + 12 + prog_info_len

        while es_start + 5 <= base + 3 + section_len - 4:
            stream_type = payload[es_start]
            es_pid      = struct.unpack(">H", payload[es_start + 1: es_start + 3])[0] & 0x1FFF
            es_info_len = struct.unpack(">H", payload[es_start + 3: es_start + 5])[0] & 0x0FFF

            # Stream types used for KLV / metadata in MPEG-TS:
            #   0x15 = Metadata PES
            #   0x06 = Private data (FFmpeg uses this for SMPTE_KLV sometimes)
            if stream_type in (0x15, 0x06):
                return es_pid

            es_start += 5 + es_info_len

    return None


# ---------------------------------------------------------------------------
# UDP / file reader
# ---------------------------------------------------------------------------

def read_udp_chunks(
    addr: str,
    port: int,
    chunk_size: int = 65536,
    max_wait_seconds: float | None = None,
) -> Iterator[bytes]:
    """Receive UDP datagrams indefinitely."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.bind(("", port))

    # Join multicast group if address is multicast
    if addr.startswith("239.") or addr.startswith("224."):
        import struct as _s
        mreq = _s.pack("4sL", socket.inet_aton(addr), socket.INADDR_ANY)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        print(f"  Joined multicast group {addr}")

    sock.settimeout(5.0)
    print(f"  Listening on udp://@{addr}:{port} …")

    start = time.monotonic()
    try:
        while True:
            if max_wait_seconds is not None and (time.monotonic() - start) >= max_wait_seconds:
                raise TimeoutError(
                    f"No UDP stream data received within {max_wait_seconds:.1f}s on {addr}:{port}"
                )
            try:
                data, _ = sock.recvfrom(chunk_size)
                yield data
            except socket.timeout:
                print("  [WARN] No data received for 5s — still waiting …")
    finally:
        sock.close()


def read_file_chunks(path: str, chunk_size: int = 65536) -> Iterator[bytes]:
    """Read a .ts file in chunks."""
    with open(path, "rb") as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            yield chunk


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--addr",    default="239.1.1.1")
    ap.add_argument("--port",    type=int, default=5004)
    ap.add_argument("--file",    default=None, help="Read from .ts file")
    ap.add_argument("--count",   type=int, default=5, help="KLV packets to decode")
    ap.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="Max seconds to wait for UDP stream data before failing (UDP mode only)",
    )
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    print("==> CamSim KLV Validator")

    # Accumulate a few TS packets to find the KLV PID via PAT/PMT
    ts_buffer = b""
    klv_pid   = None
    klv_pes_buf: dict[int, bytes] = {}   # pid → reassembly buffer
    klv_count = 0

    source = (
        read_file_chunks(args.file)
        if args.file
        else read_udp_chunks(args.addr, args.port, max_wait_seconds=args.timeout)
    )

    print(f"    Looking for KLVA data PID …")

    try:
        for chunk in source:
            ts_buffer += chunk

            # Try to identify the KLV PID once we have ~8 KB
            if klv_pid is None and len(ts_buffer) >= 8192:
                klv_pid = find_klv_pid_from_pat_pmt(ts_buffer)
                if klv_pid is not None:
                    print(f"    Found KLV PID: 0x{klv_pid:04X} ({klv_pid})")
                else:
                    # Fallback: scan all PIDs not 0x0000, 0x0001, 0x1FFF
                    # and try to parse each as KLV
                    print("    Could not identify KLV PID via PAT/PMT — scanning all PIDs")

            # Process buffered TS packets
            for pkt in iter_ts_packets(ts_buffer):
                hdr = parse_ts_header(pkt)
                pid = hdr["pid"]

                # Skip known non-data PIDs
                if pid in (0x0000, 0x0001, 0x1FFF, 0x0011):
                    continue
                # Skip video PID 0 (usually) — focus on data streams
                if pid == 0x0100 or pid == 256:
                    continue

                if klv_pid is not None and pid != klv_pid:
                    continue

                payload = extract_pes_payload(pkt)
                if not payload:
                    continue

                if hdr["pusi"]:
                    # New PES packet starts — check for KLV UL directly in payload
                    # (some muxers put KLV directly in the TS payload without a PES wrapper)
                    if ST0601_UL in payload:
                        ul_offset = payload.index(ST0601_UL)
                        klv_pes_buf[pid] = payload[ul_offset:]
                    else:
                        klv_pes_buf[pid] = payload
                else:
                    klv_pes_buf[pid] = klv_pes_buf.get(pid, b"") + payload

                # Check if we have a complete KLV packet
                buf = klv_pes_buf.get(pid, b"")
                if len(buf) < 16:
                    continue

                # Look for the ST 0601 UL in buffer
                if ST0601_UL not in buf:
                    # Not ST 0601 data — ignore this PID
                    continue

                ul_pos = buf.index(ST0601_UL)
                klv_start = ul_pos

                if klv_start + 17 > len(buf):
                    continue

                # Decode BER length
                try:
                    value_len, ber_bytes = decode_ber_length(buf, klv_start + 16)
                except ValueError:
                    continue

                total_len = 16 + ber_bytes + value_len
                if klv_start + total_len > len(buf):
                    continue  # not yet complete

                klv_packet = buf[klv_start: klv_start + total_len]
                klv_pes_buf[pid] = buf[klv_start + total_len:]

                if klv_pid is None:
                    klv_pid = pid
                    print(f"    Auto-detected KLV PID: 0x{pid:04X} ({pid})")

                result = parse_klv_local_set(klv_packet, verbose=args.verbose)
                klv_count += 1
                print_klv_result(result, klv_count)

                if klv_count >= args.count:
                    print(f"\n==> Decoded {klv_count} KLV packets — done.")
                    return

            # Keep only the last partial TS packet in the buffer
            remainder = len(ts_buffer) % TS_PACKET_SIZE
            ts_buffer = ts_buffer[-remainder:] if remainder else b""
    except TimeoutError as e:
        print(f"\n[FAIL] {e}")
        sys.exit(1)

    if klv_count == 0:
        print("\n[FAIL] No MISB ST 0601 KLV packets found.")
        print("       Is Phase 4 (KLV metadata injection) enabled?")
        print("       Try --verbose for debug output.")
        sys.exit(1)


if __name__ == "__main__":
    main()
