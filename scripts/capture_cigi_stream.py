#!/usr/bin/env python3
"""
capture_cigi_stream.py — capture raw CIGI UDP datagrams to JSONL.

Output format (one line per datagram):
  {"t_us":12345,"src":"127.0.0.1:50000","data_hex":"..."}
"""

import argparse
import json
import socket
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bind-host", default="0.0.0.0")
    ap.add_argument("--bind-port", type=int, default=8888)
    ap.add_argument("--output", required=True, help="Path to JSONL capture file")
    ap.add_argument("--duration", type=float, default=0.0, help="Seconds to capture (0=until Ctrl-C)")
    ap.add_argument("--max-packets", type=int, default=0, help="Stop after N packets (0=unlimited)")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind_host, args.bind_port))
    sock.settimeout(1.0)

    print(f"[capture] listening on {args.bind_host}:{args.bind_port}")
    print(f"[capture] writing {args.output}")

    start = time.monotonic()
    count = 0
    with open(args.output, "w", encoding="utf-8") as f:
        try:
            while True:
                if args.duration > 0 and (time.monotonic() - start) >= args.duration:
                    break
                if args.max_packets > 0 and count >= args.max_packets:
                    break
                try:
                    data, src = sock.recvfrom(65535)
                except socket.timeout:
                    continue
                now = time.monotonic()
                rec = {
                    "t_us": int((now - start) * 1_000_000),
                    "src": f"{src[0]}:{src[1]}",
                    "data_hex": data.hex(),
                }
                f.write(json.dumps(rec, separators=(",", ":")) + "\n")
                count += 1
                if count % 100 == 0:
                    print(f"  captured={count}")
        except KeyboardInterrupt:
            print()

    sock.close()
    print(f"[capture] done packets={count}")


if __name__ == "__main__":
    main()
