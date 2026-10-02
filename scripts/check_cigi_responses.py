#!/usr/bin/env -S uv run
"""
check_cigi_responses.py - verify CamSim IG->Host CIGI response traffic.

By default this validates Start-of-Frame (opcode 101) packets on the response
socket. Optionally require terrain/LOS query responses (102/103/104/105) or the
Sensor Extended Response (107) that carries the frame centre every tick.

CamSim packs its responses with CCL in its native byte order (little-endian on
x86/ARM); the SOF's Byte Swap Magic (bytes 6-7) says which, so the parsers read
the order from it.

Layouts (CIGI 3.3, CCL V3_2 Pack()):
  102 HAT/HOT Response, 16 B: 2-3 ID, 4 Valid (bit 0) | Type (bit 1, 1 = HOT) |
      Host Frame LSN (bits 4-7), 8 height (f64)
  103 HAT/HOT Extended Response, 40 B: 2-3 ID, 4 Valid (bit 0), 8 HAT (f64),
      16 HOT (f64), 24 Material (u32), 28 Normal Az, 32 Normal El (f32)
  107 Sensor Extended Response, 48 B: 2-3 View ID, 4 Sensor ID, 5 Status
      (bits 0-1), 6-7 Entity ID, 8-11 Gate size, 12/16 Gate offsets (f32),
      20 Frame Counter (u32), 24 Track Lat, 32 Lon, 40 Alt (f64, ellipsoid)
"""

import argparse
import socket
import struct
import time

QUERY_RESPONSES = (102, 103, 104, 105)


def parse_packet_ids(datagram: bytes) -> list[int]:
    ids: list[int] = []
    i = 0
    n = len(datagram)
    while i + 2 <= n:
        pkt_id = datagram[i]
        pkt_size = datagram[i + 1]
        if pkt_size < 2 or i + pkt_size > n:
            break
        ids.append(pkt_id)
        i += pkt_size
    return ids


def byte_order(datagram: bytes) -> str:
    """'<' or '>' from the leading SOF's Byte Swap Magic (0x8000 in sender order)."""
    if len(datagram) >= 8 and datagram[0] == 101 and datagram[6:8] == b"\x00\x80":
        return "<"
    return ">"


def parse_responses(datagram: bytes) -> dict:
    """Packet IDs plus the decoded SOF frame, HAT/HOT and frame-centre packets."""
    e = byte_order(datagram)
    out: dict = {"ids": [], "sof_frame": None, "hat_hot": [], "sensor_ext": []}
    i, n = 0, len(datagram)
    while i + 2 <= n:
        op, size = datagram[i], datagram[i + 1]
        if size < 2 or i + size > n:
            break
        p = datagram[i : i + size]
        out["ids"].append(op)
        if op == 101 and size >= 12:
            out["sof_frame"] = struct.unpack_from(e + "I", p, 8)[0]
        elif op == 102 and size >= 16:
            rid = struct.unpack_from(e + "H", p, 2)[0]
            (h,) = struct.unpack_from(e + "d", p, 8)
            kind = "hot" if p[4] & 0x02 else "hat"
            out["hat_hot"].append({"op": 102, "id": rid, "valid": bool(p[4] & 1), kind: h})
        elif op == 103 and size >= 40:
            rid = struct.unpack_from(e + "H", p, 2)[0]
            hat, hot = struct.unpack_from(e + "dd", p, 8)
            out["hat_hot"].append({"op": 103, "id": rid, "valid": bool(p[4] & 1), "hat": hat, "hot": hot})
        elif op == 107 and size >= 48:
            sensor_id, status = p[4], p[5] & 0x03
            frame = struct.unpack_from(e + "I", p, 20)[0]
            lat, lon, alt = struct.unpack_from(e + "ddd", p, 24)
            out["sensor_ext"].append(
                {"sensor_id": sensor_id, "status": status, "frame": frame, "lat": lat, "lon": lon, "alt": alt}
            )
        i += size
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0", help="Bind host/interface")
    ap.add_argument("--port", type=int, default=8889, help="CIGI response UDP port")
    ap.add_argument("--timeout", type=float, default=10.0, help="Max wait seconds")
    ap.add_argument(
        "--min-packets", type=int, default=3, help="Minimum datagrams to receive"
    )
    ap.add_argument(
        "--require-query-response",
        action="store_true",
        help="Require at least one HAT/HOT or LOS response opcode (102-105)",
    )
    ap.add_argument(
        "--require-sensor-response",
        action="store_true",
        help="Require at least one Sensor Extended Response (107, frame centre)",
    )
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind((args.host, args.port))
        sock.settimeout(0.5)
    except OSError as e:
        print(f"[FAIL] Unable to bind {args.host}:{args.port}: {e}")
        return 1

    deadline = time.monotonic() + max(0.5, args.timeout)
    datagrams = 0
    sof_count = 0
    query_resp_count = 0
    counts = {op: 0 for op in (101, 102, 103, 104, 105, 107)}
    last_hat_hot = None
    last_centre = None

    while time.monotonic() < deadline:
        try:
            data, _ = sock.recvfrom(65535)
        except TimeoutError:
            continue
        datagrams += 1
        r = parse_responses(data)
        for op in r["ids"]:
            if op in counts:
                counts[op] += 1
        sof_count = counts[101]
        query_resp_count = sum(counts[op] for op in QUERY_RESPONSES)
        if r["hat_hot"]:
            last_hat_hot = r["hat_hot"][-1]
        if r["sensor_ext"]:
            last_centre = r["sensor_ext"][-1]
        if (
            datagrams >= args.min_packets
            and sof_count > 0
            and (not args.require_query_response or query_resp_count > 0)
            and (not args.require_sensor_response or counts[107] > 0)
        ):
            break

    sock.close()

    if datagrams < args.min_packets:
        print(
            f"[FAIL] Received only {datagrams} response datagrams (expected >= {args.min_packets})"
        )
        return 1
    if sof_count == 0:
        print("[FAIL] No SOF opcode 101 packets found in response datagrams")
        return 1
    if args.require_query_response and query_resp_count == 0:
        print("[FAIL] No query response opcode 102-105 packets observed")
        return 1
    if args.require_sensor_response and counts[107] == 0:
        print("[FAIL] No Sensor Extended Response opcode 107 packets observed")
        return 1

    print(
        f"[PASS] CIGI responses ok: datagrams={datagrams} "
        f"sof={sof_count} query_responses={query_resp_count} "
        + " ".join(f"{op}={n}" for op, n in counts.items() if op != 101)
    )
    if last_hat_hot is not None:
        print(f"       last HAT/HOT: {last_hat_hot}")
    if last_centre is not None:
        c = last_centre
        print(
            f"       frame centre (107, frame {c['frame']}): lat={c['lat']:.7f} lon={c['lon']:.7f} "
            f"alt={c['alt']:.1f} m ellipsoid (one frame old)"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
