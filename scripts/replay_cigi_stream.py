#!/usr/bin/env python3
"""
replay_cigi_stream.py — deterministic replay of JSONL CIGI captures.

Expected input line format (from capture_cigi_stream.py):
  {"t_us":12345,"data_hex":"..."}
"""

import argparse
import json
import socket
import time


def load_capture(path: str) -> list[tuple[int, bytes]]:
    rows: list[tuple[int, bytes]] = []
    with open(path, "r", encoding="utf-8") as f:
        for line_no, line in enumerate(f, start=1):
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            if "t_us" not in rec or "data_hex" not in rec:
                raise ValueError(f"{path}:{line_no}: missing t_us/data_hex")
            rows.append((int(rec["t_us"]), bytes.fromhex(rec["data_hex"])))
    rows.sort(key=lambda x: x[0])
    return rows


def replay_once(sock: socket.socket, addr: tuple[str, int], rows: list[tuple[int, bytes]], speed: float) -> int:
    if not rows:
        return 0
    start = time.monotonic()
    sent = 0
    speed = max(0.0001, speed)
    for t_us, payload in rows:
        target = start + (t_us / 1_000_000.0) / speed
        while True:
            now = time.monotonic()
            remaining = target - now
            if remaining <= 0:
                break
            time.sleep(min(0.01, remaining))
        sock.sendto(payload, addr)
        sent += 1
    return sent


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--input", required=True, help="JSONL capture file")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8888)
    ap.add_argument("--speed", type=float, default=1.0, help="Replay speed multiplier (1.0=real-time)")
    ap.add_argument("--loops", type=int, default=1, help="Replay loops (0=infinite)")
    args = ap.parse_args()

    rows = load_capture(args.input)
    if not rows:
        raise SystemExit("No packets found in capture")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    addr = (args.host, args.port)
    print(
        f"[replay] input={args.input} packets={len(rows)} target={args.host}:{args.port} "
        f"speed={args.speed:.3f} loops={args.loops}"
    )

    total_sent = 0
    loop = 0
    try:
        while args.loops == 0 or loop < args.loops:
            loop += 1
            sent = replay_once(sock, addr, rows, args.speed)
            total_sent += sent
            print(f"  loop={loop} sent={sent} total={total_sent}")
    except KeyboardInterrupt:
        print()

    sock.close()
    print(f"[replay] done total_sent={total_sent}")


if __name__ == "__main__":
    main()
