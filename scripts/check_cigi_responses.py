#!/usr/bin/env python3
"""
check_cigi_responses.py - verify CamSim IG->Host CIGI response traffic.

By default this validates Start-of-Frame (opcode 101) packets on the response
socket. Optionally require terrain/LOS query responses (102/104).
"""

import argparse
import socket
import time


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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="0.0.0.0", help="Bind host/interface")
    ap.add_argument("--port", type=int, default=8889, help="CIGI response UDP port")
    ap.add_argument("--timeout", type=float, default=10.0, help="Max wait seconds")
    ap.add_argument("--min-packets", type=int, default=3, help="Minimum datagrams to receive")
    ap.add_argument(
        "--require-query-response",
        action="store_true",
        help="Require at least one HAT/HOT or LOS response opcode (102 or 104)",
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

    while time.monotonic() < deadline:
        try:
            data, _ = sock.recvfrom(65535)
        except socket.timeout:
            continue
        datagrams += 1
        pkt_ids = parse_packet_ids(data)
        sof_count += sum(1 for x in pkt_ids if x == 101)
        query_resp_count += sum(1 for x in pkt_ids if x in (102, 104))
        if datagrams >= args.min_packets and sof_count > 0:
            if not args.require_query_response or query_resp_count > 0:
                break

    sock.close()

    if datagrams < args.min_packets:
        print(f"[FAIL] Received only {datagrams} response datagrams (expected >= {args.min_packets})")
        return 1
    if sof_count == 0:
        print("[FAIL] No SOF opcode 101 packets found in response datagrams")
        return 1
    if args.require_query_response and query_resp_count == 0:
        print("[FAIL] No query response opcode 102/104 packets observed")
        return 1

    print(
        f"[PASS] CIGI responses ok: datagrams={datagrams} "
        f"sof={sof_count} query_responses={query_resp_count}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

