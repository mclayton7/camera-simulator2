#!/usr/bin/env python3
"""Synthetic X-Plane truth sender: hitl/PROTOCOL.md section 1 (CSTR) datagrams.

Lets the IG host and CamSim run without X-Plane:

    uv run --project hitl hitl/tools/fake_xplane.py orbit --lat 37.62 --lon -122.38 --alt 300
    uv run --project hitl hitl/tools/fake_xplane.py straight --heading 90 --speed 30
    uv run --project hitl hitl/tools/fake_xplane.py parked --alt 4 --agl 2

Scenarios: orbit (coordinated turn round --lat/--lon at --radius), straight
(straight and level from --lat/--lon on --heading), parked (static). Altitudes
are MSL, like X-Plane's `elevation`. Weather is added once a second.
"""

from __future__ import annotations

import argparse
import math
import socket
import sys
import time
from datetime import UTC, datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from camsim_hitl.geodesy import offset_ned, radii  # noqa: E402
from camsim_hitl.truth import FLAG_PAUSED, TruthSample, Weather, pack_truth  # noqa: E402

G = 9.80665


class Scenario:
    def __init__(self, args):
        self.a = args

    def state(self, t: float) -> dict:
        a = self.a
        if a.scenario == "parked":
            return dict(lat=a.lat, lon=a.lon, alt=a.alt, psi=a.heading, theta=0.0, phi=0.0,
                        p=0.0, q=0.0, r=0.0, vn=0.0, ve=0.0, vd=0.0, agl=a.agl)
        if a.scenario == "straight":
            hd = math.radians(a.heading)
            vn, ve = a.speed * math.cos(hd), a.speed * math.sin(hd)
            lat, lon, alt = offset_ned(a.lat, a.lon, a.alt, vn * t, ve * t, 0.0)
            return dict(lat=lat, lon=lon, alt=alt, psi=a.heading, theta=a.pitch, phi=0.0,
                        p=0.0, q=0.0, r=0.0, vn=vn, ve=ve, vd=0.0, agl=a.agl)
        # orbit: clockwise coordinated turn round the centre
        w = a.speed / a.radius  # rad/s
        ang = w * t  # bearing of the aircraft from the centre
        lat, lon, alt = offset_ned(a.lat, a.lon, a.alt, a.radius * math.cos(ang), a.radius * math.sin(ang), 0.0)
        psi = math.degrees(ang) + 90.0  # tangent, clockwise
        phi = math.atan(a.speed * w / G)
        vn, ve = -a.speed * math.sin(ang), a.speed * math.cos(ang)
        return dict(lat=lat, lon=lon, alt=alt, psi=psi % 360.0, theta=0.0, phi=math.degrees(phi),
                    p=0.0, q=w * math.sin(phi), r=w * math.cos(phi), vn=vn, ve=ve, vd=0.0, agl=a.agl)


def weather(args) -> Weather:
    return Weather(
        visibility_m=args.visibility,
        temperature_c=15.0,
        humidity_pct=60.0,
        wind_speed=5.0,
        wind_dir_degt=270.0,
        baro_inhg=29.92,
        cloud_base_msl_m=(args.cloud_base, 6000.0, 0.0),
        cloud_tops_msl_m=(args.cloud_base + 400.0, 6500.0, 0.0),
        cloud_coverage=(3.0, 1.0, 0.0),
        cloud_type=(float(args.cloud_type), 1.0, 0.0),
        wave_amplitude=0.5,
        wave_length=30.0,
        wave_speed=6.8,
        wave_dir=90.0,
    )


def make_sample(args, scen: Scenario, t: float, seq: int, with_weather: bool, paused: bool = False) -> TruthSample:
    s = scen.state(t)
    now = datetime.now(UTC)
    zulu = now.hour * 3600 + now.minute * 60 + now.second + now.microsecond / 1e6
    local = (zulu + args.tz_hours * 3600.0) % 86400.0
    local_dt = datetime.fromtimestamp(now.timestamp() + args.tz_hours * 3600.0, UTC)
    m, n = radii(s["lat"])
    return TruthSample(
        flags=FLAG_PAUSED if paused else 0,
        cycle=seq,
        seq=seq,
        mono_ns=time.monotonic_ns(),
        latitude=s["lat"],
        longitude=s["lon"],
        elevation=s["alt"],
        terrain_msl=s["alt"] - s["agl"],
        true_psi=s["psi"],
        true_theta=s["theta"],
        true_phi=s["phi"],
        p=s["p"],
        q=s["q"],
        r=s["r"],
        local_vx=s["ve"],  # OpenGL: east, up, south
        local_vy=-s["vd"],
        local_vz=-s["vn"],
        true_airspeed=args.speed if args.scenario != "parked" else 0.0,
        groundspeed=math.hypot(s["vn"], s["ve"]),
        indicated_airspeed=(args.speed if args.scenario != "parked" else 0.0) / 0.514444,
        mag_psi=(s["psi"] - args.mag_var) % 360.0,
        y_agl=s["agl"],
        sim_speed=1.0,
        sim_speed_actual_ogl=1.0,
        zulu_time_sec=zulu,
        local_time_sec=local,
        local_month=local_dt.month,
        local_day=local_dt.day,
        use_system_time=1,
        earth_radius_m=6378145.0,
        weather=weather(args) if with_weather else None,
    )


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenario", choices=["orbit", "straight", "parked"], nargs="?", default="orbit")
    ap.add_argument("--dest", default="127.0.0.1:49300", help="IG host truth address (default 127.0.0.1:49300)")
    ap.add_argument("--rate", type=float, default=60.0, help="datagrams per second (X-Plane frame rate)")
    ap.add_argument("--lat", type=float, default=37.6213)
    ap.add_argument("--lon", type=float, default=-122.379)
    ap.add_argument("--alt", type=float, default=300.0, help="m MSL")
    ap.add_argument("--agl", type=float, default=None, help="y_agl (default: alt)")
    ap.add_argument("--speed", type=float, default=25.0, help="m/s")
    ap.add_argument("--radius", type=float, default=400.0, help="orbit radius m")
    ap.add_argument("--heading", type=float, default=0.0, help="deg true (straight, parked)")
    ap.add_argument("--pitch", type=float, default=0.0, help="deg (straight)")
    ap.add_argument("--tz-hours", type=float, default=None, help="local time offset (default lon/15)")
    ap.add_argument("--mag-var", type=float, default=13.0, help="magnetic variation, deg east")
    ap.add_argument("--visibility", type=float, default=20000.0)
    ap.add_argument("--cloud-base", type=float, default=1500.0)
    ap.add_argument("--cloud-type", type=int, default=2, help="X-Plane cloud_type of layer 0 (0-5)")
    ap.add_argument("--duration", type=float, default=0.0, help="seconds (0 = forever)")
    ap.add_argument("--pause-at", type=float, default=None, help="set the paused flag from this time ...")
    ap.add_argument("--pause-for", type=float, default=5.0, help="... for this long")
    args = ap.parse_args(argv)
    if args.agl is None:
        args.agl = args.alt
    if args.tz_hours is None:
        args.tz_hours = round(args.lon / 15.0)
    host, _, port = args.dest.rpartition(":")
    dest = (host or "127.0.0.1", int(port))
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    scen = Scenario(args)
    period = 1.0 / args.rate
    t0 = time.monotonic()
    next_t = t0
    seq = 0
    last_weather = -1.0
    sim_t = 0.0
    last = t0
    print(f"fake_xplane: {args.scenario} -> {dest[0]}:{dest[1]} at {args.rate:.0f} Hz (Ctrl-C to stop)")
    try:
        while True:
            now = time.monotonic()
            el = now - t0
            if args.duration and el >= args.duration:
                break
            paused = args.pause_at is not None and args.pause_at <= el < args.pause_at + args.pause_for
            if not paused:
                sim_t += now - last
            last = now
            ww = el - last_weather >= 1.0
            if ww:
                last_weather = el
            seq += 1
            sock.sendto(pack_truth(make_sample(args, scen, sim_t, seq, ww, paused)), dest)
            next_t += period
            time.sleep(max(0.0, next_t - time.monotonic()))
    except KeyboardInterrupt:
        pass
    print(f"fake_xplane: sent {seq} datagrams")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
