"""One-off: crop PROJ's GEOID18 grid (public domain, NOAA) to Pendleton + 100 km for offline datum tests.
Keeps PROJ's grid tags (TYPE=VERTICAL_OFFSET_GEOGRAPHIC_TO_VERTICAL), so vgridshift accepts the crop.

    uv run --project scripts/scene python scripts/scene/tests/fixtures/make_geoid_fixture.py
"""

import urllib.request
from pathlib import Path

import rasterio
from rasterio.windows import from_bounds

URL = "https://cdn.proj.org/us_noaa_g2018u0.tif"
OUT = Path(__file__).parent / "grids" / "us_noaa_g2018u0.tif"


def main() -> None:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    src = OUT.with_suffix(".full.tif")
    urllib.request.urlretrieve(URL, src)
    with rasterio.open(src) as ds:  # longitudes are positive east (230..300)
        w = from_bounds(-118.8 + 360, 32.2, -116.0 + 360, 34.6, ds.transform).round_offsets().round_lengths()
        data = ds.read(window=w)
        prof = ds.profile.copy()
        prof.update(
            width=data.shape[2], height=data.shape[1], transform=ds.window_transform(w), compress="deflate", tiled=False
        )
        prof.pop("blockxsize", None)
        prof.pop("blockysize", None)
        with rasterio.open(OUT, "w", **prof) as out:
            out.write(data)
            out.update_tags(**ds.tags())
            out.update_tags(1, **ds.tags(1))
    src.unlink()
    print(OUT, OUT.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
