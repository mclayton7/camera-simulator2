#!/usr/bin/env python3
# Copyright CamSim Contributors. All Rights Reserved.
"""Decode one CamSimTruth datagram (dumped by camsim_truth_test, frame 0 of the
"full" scenario) with the struct format from hitl/PROTOCOL.md and check it
against the values the harness's fake X-Plane published.

usage: decode_check.py <sample_datagram.bin>
"""
import struct
import sys

FMT = struct.Struct("<4sHHIIQdddd" + "f" * 18 + "BBBB" + "f" * 23)
NAMES = (
    "magic version flags cycle seq mono_ns latitude longitude elevation terrain_msl "
    "true_psi true_theta true_phi p q r local_vx local_vy local_vz true_airspeed "
    "groundspeed indicated_airspeed mag_psi y_agl sim_speed sim_speed_actual_ogl "
    "zulu_time_sec local_time_sec local_month local_day use_system_time reserved "
    "earth_radius_m visibility_m temperature_c humidity_pct wind_speed wind_dir_degt baro_inhg "
    "cloud_base0 cloud_base1 cloud_base2 cloud_tops0 cloud_tops1 cloud_tops2 "
    "cloud_cov0 cloud_cov1 cloud_cov2 cloud_type0 cloud_type1 cloud_type2 "
    "wave_amplitude wave_length wave_speed wave_dir"
).split()

# frame 0 values from tests/test_plugin.c FIELDS[] (all exact in float32)
EXPECT = {
    "magic": b"CSTR", "version": 1, "flags": 0b1100, "seq": 0, "mono_ns": 5_000_000_000,
    "latitude": 34.123456789012345, "longitude": -116.987654321098765, "elevation": 812.3456789,
    "terrain_msl": 600.25,
    "true_psi": 271.5, "true_theta": 3.25, "true_phi": -12.75, "p": 0.125, "q": -0.25, "r": 0.375,
    "local_vx": 41.5, "local_vy": -2.25, "local_vz": -17.75, "true_airspeed": 45.5, "groundspeed": 44.25,
    "indicated_airspeed": 86.5, "mag_psi": 259.25, "y_agl": 304.5, "sim_speed": 1.0,
    "sim_speed_actual_ogl": 0.9921875, "zulu_time_sec": 61200.5, "local_time_sec": 32400.5,
    "local_month": 7, "local_day": 4, "use_system_time": 1, "reserved": 0, "earth_radius_m": 6378145.0,
    "visibility_m": 16093.0, "temperature_c": 21.5, "humidity_pct": 40.5, "wind_speed": 5.25,
    "wind_dir_degt": 245.5, "baro_inhg": 29.875,
    "cloud_base0": 1500.5, "cloud_base1": 3000.5, "cloud_base2": 6000.5,
    "cloud_tops0": 2000.25, "cloud_tops1": 4000.25, "cloud_tops2": 7000.25,
    "cloud_cov0": 2.5, "cloud_cov1": 4.0, "cloud_cov2": 0.5,
    "cloud_type0": 2.0, "cloud_type1": 3.0, "cloud_type2": 1.0,
    "wave_amplitude": 0.75, "wave_length": 22.5, "wave_speed": 5.5, "wave_dir": 135.0,
}


def main() -> int:
    data = open(sys.argv[1], "rb").read()
    assert FMT.size == 224, FMT.size
    assert len(NAMES) == len(FMT.unpack(bytes(224))), "name table out of step with the format"
    if len(data) != FMT.size:
        print(f"FAIL: datagram is {len(data)} bytes, want {FMT.size}")
        return 1
    rec = dict(zip(NAMES, FMT.unpack(data)))
    bad = [f"{k}: got {rec[k]!r}, want {v!r}" for k, v in EXPECT.items() if rec[k] != v]
    if rec["cycle"] < 1:
        bad.append(f"cycle: {rec['cycle']}")
    for line in bad:
        print("FAIL", line)
    if not bad:
        print(f"OK: {len(EXPECT) + 1} fields decoded with struct {FMT.format!r}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
