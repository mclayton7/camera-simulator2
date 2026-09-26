#!/usr/bin/env python3
"""make_egm96_dac.py — build the EGM96 geoid grid CamSim uses for MSL altitudes.

Downloads NGA's 15-arcminute EGM96 geoid grid (public domain) and writes it in
the WW15MGH.DAC layout that Cesium Native's
CesiumGeospatial::EarthGravitationalModel1996Grid::fromBuffer() reads:
721 rows (90N to 90S) x 1440 columns (0E to 359.75E), big-endian int16
geoid undulation in centimetres.

NGA distributes the grid as ASCII (WW15MGH.GRD: a header line, then 721 rows
of 1441 values in metres, including a duplicate 360E column), so this script
converts it. The output is checked into git; re-run only to regenerate it.

Usage:
    python3 scripts/make_egm96_dac.py [--grd WW15MGH.GRD] [--out PATH]
"""

import argparse
import io
import struct
import sys
import urllib.request
import zipfile
from pathlib import Path

NGA_URL = "https://earth-info.nga.mil/php/download.php?file=egm-96interpolation"
ROWS, COLS = 721, 1440
DEFAULT_OUT = (
    Path(__file__).resolve().parent.parent
    / "unreal_project/CamSimTest/Content/NonUFS/Geoid/WW15MGH.DAC"
)


def load_grd_text(grd_path):
    if grd_path:
        return Path(grd_path).read_text()
    print(f"Downloading {NGA_URL} ...", file=sys.stderr)
    with urllib.request.urlopen(NGA_URL, timeout=120) as resp:
        archive = zipfile.ZipFile(io.BytesIO(resp.read()))
    return archive.read("WW15MGH.GRD").decode("ascii")


def main():
    ap = argparse.ArgumentParser(description="Build the EGM96 WW15MGH.DAC geoid grid.")
    ap.add_argument("--grd", help="Use a local WW15MGH.GRD instead of downloading")
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    args = ap.parse_args()

    tokens = load_grd_text(args.grd).split()
    header = [float(t) for t in tokens[:6]]
    if header != [-90.0, 90.0, 0.0, 360.0, 0.25, 0.25]:
        sys.exit(f"Unexpected WW15MGH.GRD header: {header}")
    values = [float(t) for t in tokens[6:]]
    if len(values) != ROWS * (COLS + 1):
        sys.exit(f"Expected {ROWS * (COLS + 1)} grid values, found {len(values)}")

    out = bytearray()
    for row in range(ROWS):
        start = row * (COLS + 1)
        for metres in values[start : start + COLS]:  # drop the duplicate 360E column
            out += struct.pack(">h", round(metres * 100))

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_bytes(out)
    print(f"Wrote {len(out)} bytes to {args.out}")


if __name__ == "__main__":
    main()
