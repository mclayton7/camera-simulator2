# Copyright CamSim Contributors. All Rights Reserved.
"""Moon albedo map for the sky dome's moon disc (ROADMAP 6A): NASA SVS CGI Moon Kit colour map (LROC WAC Hapke-
normalised mosaic, 643/566/415 nm as R/G/B; 16-bit sRGB, 4096 x 2048) -> Content/NonUFS/Sky/moon_albedo.pgm.

The sRGB encoding is inverted, the linear Rec. 709 luminance taken, averaged over 4 x 4 blocks (1024 x 512) and scaled
so the brightest texel is 255 (8-bit binary PGM, linear: only relative albedo matters, the dome normalises the disc to
the Krisciunas & Schaefer illuminance). Equirectangular, longitude -180 .. 180 left to right (0 = the mean sub-Earth
meridian at the centre), latitude +90 at the top: the IAU mean Earth / polar axis frame of the LROC products.

    uv run --with tifffile --with imagecodecs --with numpy python scripts/sky/make_moon_albedo.py IN.tif OUT.pgm
"""

import sys

import numpy as np
import tifffile

OUT_W, OUT_H = 1024, 512


def main() -> None:
    src, dst = sys.argv[1], sys.argv[2]
    a = tifffile.imread(src).astype(np.float64) / 65535.0
    if a.ndim != 3 or a.shape[2] < 3 or a.shape[1] != 2 * a.shape[0]:
        sys.exit(f"unexpected image shape {a.shape}")
    lin = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    y = lin[..., 0] * 0.2126 + lin[..., 1] * 0.7152 + lin[..., 2] * 0.0722
    f = y.shape[0] // OUT_H
    y = y[: OUT_H * f, : OUT_W * f].reshape(OUT_H, f, OUT_W, f).mean(axis=(1, 3))
    q = np.clip(np.floor(y / y.max() * 255.0 + 0.5), 0, 255).astype(np.uint8)
    with open(dst, "wb") as fh:
        fh.write(b"P5\n%d %d\n255\n" % (OUT_W, OUT_H))
        fh.write(q.tobytes())
    print(f"{dst}: {OUT_W}x{OUT_H}, mean {q.mean():.1f}, min {q.min()}, max {q.max()}")


if __name__ == "__main__":
    main()
