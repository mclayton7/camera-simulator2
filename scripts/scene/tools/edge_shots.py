"""R1 chunk 1: stitch a built package's imagery tiles into labelled crops of the Pendleton bbox edges (and the
whole bbox), optionally beside the same crops from an earlier package.

    uv run --project scripts/scene python scripts/scene/tools/edge_shots.py OUT PKG [--before OLD_PKG]

A tile missing at the crop's zoom (the ring stops at z10, Sentinel-2 at z13) is taken from its nearest existing
parent and upsampled; magenta only where no ancestor exists either."""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw

from camsim_scene.tiling import tile_size_deg

CROPS = {  # name: (zoom, W, S, E, N)
    "overview": (12, -117.70, 33.13, -117.17, 33.58),
    "west_edge": (14, -117.72, 33.40, -117.56, 33.47),
    "south_edge": (14, -117.42, 33.10, -117.30, 33.22),
}
MAGENTA = (255, 0, 255)


def load_tile(pkg: Path, z: int, x: int, y: int) -> Image.Image:
    """The 256 px tile, or the matching sub-region of the nearest existing parent, upsampled."""
    for k in range(min(z, 8) + 1):
        p = pkg / "imagery" / str(z - k) / str(x >> k) / f"{y >> k}.jpg"
        if not p.exists():
            continue
        img = Image.open(p).convert("RGB")
        if k == 0:
            return img
        n, size = 1 << k, 256 >> k
        fx, fy = x - ((x >> k) << k), y - ((y >> k) << k)  # y counts from the south
        left, top = fx * size, (n - 1 - fy) * size
        return img.crop((left, top, left + size, top + size)).resize((256, 256), Image.BICUBIC)
    return Image.new("RGB", (256, 256), MAGENTA)


def mosaic(pkg: Path, z: int, w: float, s: float, e: float, n: float) -> Image.Image:
    t = tile_size_deg(z)
    x0, x1 = math.floor((w + 180) / t), math.floor((e + 180) / t)
    y0, y1 = math.floor((s + 90) / t), math.floor((n + 90) / t)
    img = Image.new("RGB", ((x1 - x0 + 1) * 256, (y1 - y0 + 1) * 256), MAGENTA)
    for x in range(x0, x1 + 1):
        for y in range(y0, y1 + 1):
            img.paste(load_tile(pkg, z, x, y), ((x - x0) * 256, (y1 - y) * 256))
    px = 256 / t
    left, top = (w - (x0 * t - 180)) * px, (((y1 + 1) * t - 90) - n) * px
    return img.crop((round(left), round(top), round(left + (e - w) * px), round(top + (n - s) * px)))


def labelled(img: Image.Image, text: str, width: int = 1600) -> Image.Image:
    if img.width > width:
        img = img.resize((width, round(img.height * width / img.width)), Image.LANCZOS)
    out = Image.new("RGB", (img.width, img.height + 36), (0, 0, 0))
    out.paste(img, (0, 36))
    ImageDraw.Draw(out).text((8, 10), text, fill=(255, 255, 255))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", type=Path)
    ap.add_argument("pkg", type=Path)
    ap.add_argument("--before", type=Path)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    for name, (z, *box) in CROPS.items():
        after = labelled(mosaic(a.pkg, z, *box), f"{name} z{z} -- {a.pkg.name}")
        if a.before:
            before = labelled(mosaic(a.before, z, *box), f"{name} z{z} -- {a.before.name}")
            both = Image.new("RGB", (after.width, before.height + after.height))
            both.paste(before, (0, 0))
            both.paste(after, (0, before.height))
            after = both
        path = a.out / f"{name}.jpg"
        after.save(path, quality=90)
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
