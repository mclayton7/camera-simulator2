"""Spike: NAIP (Planetary Computer, 2022) -> geodetic TMS (EPSG:4326, 256 px, z0 = 2x1) with tilemapresource.xml.
Applies the NAD83(2011) -> ITRF2014 horizontal shift as a constant."""
import argparse, os, math, time, numpy as np, rasterio, pystac_client, planetary_computer as pc, pyproj
from rasterio.warp import reproject, Resampling
from rasterio.transform import from_origin
from PIL import Image
ap = argparse.ArgumentParser(); ap.add_argument('--bbox', type=float, nargs=4); ap.add_argument('--year', default='2022')
ap.add_argument('--minz', type=int, default=10); ap.add_argument('--maxz', type=int, default=17); ap.add_argument('--out')
a = ap.parse_args(); W, S, E, N = a.bbox
tr = pyproj.Transformer.from_crs('EPSG:6318', 'EPSG:7912', always_xy=True)
lo2, la2, _ = tr.transform((W+E)/2, (S+N)/2, 0); dlo, dla = lo2-(W+E)/2, la2-(S+N)/2
cat = pystac_client.Client.open("https://planetarycomputer.microsoft.com/api/stac/v1", modifier=pc.sign_inplace)
items = [i for i in cat.search(collections=["naip"], bbox=[W, S, E, N]).items() if i.properties['naip:year'] == a.year]
print(len(items), 'NAIP items', [i.id for i in items])
z = a.maxz; res = 180.0 / 256 / (1 << z)
tx0 = int((W + dlo + 180) / (256 * res)); tx1 = int((E + dlo + 180) / (256 * res))
ty0 = int((S + dla + 90) / (256 * res)); ty1 = int((N + dla + 90) / (256 * res))
mw = (tx1 - tx0 + 1) * 256; mh = (ty1 - ty0 + 1) * 256
west = -180 + tx0 * 256 * res; north = -90 + (ty1 + 1) * 256 * res
# the grid is in ITRF lon/lat; read the source in NAD83 lon/lat = ITRF minus the shift
T = from_origin(west - dlo, north - dla, res, res)
mos = np.zeros((3, mh, mw), np.uint8); t0 = time.time()
for it in items:
    with rasterio.open(it.assets['image'].href) as ds:
        for b in range(3):
            tmp = np.zeros((mh, mw), np.uint8)
            reproject(rasterio.band(ds, b + 1), tmp, dst_transform=T, dst_crs='EPSG:6318', resampling=Resampling.bilinear, src_nodata=0, dst_nodata=0)
            m = tmp > 0; mos[b][m] = tmp[m]
print(f'mosaic {mw}x{mh} in {time.time()-t0:.0f}s')
img = Image.fromarray(np.moveaxis(mos, 0, -1)); n = 0; nbytes = 0
for zz in range(a.maxz, a.minz - 1, -1):
    f = 1 << (a.maxz - zz)
    im = img if f == 1 else img.resize((max(1, mw // f), max(1, mh // f)), Image.LANCZOS)
    ox, oy = tx0 * 256 // f, (ty1 + 1) * 256 // f  # pixel origin of the mosaic at this zoom (x from west, y-top in TMS rows)
    for X in range((tx0 * 256) // (256 * f), (tx1 * 256 + 255) // (256 * f) + 1):
        for Y in range((ty0 * 256) // (256 * f), ((ty1 + 1) * 256 - 1) // (256 * f) + 1):
            px = X * 256 - ox; py = oy - (Y + 1) * 256
            tile = Image.new('RGB', (256, 256)); tile.paste(im.crop((px, py, px + 256, py + 256)), (0, 0))
            d = os.path.join(a.out, str(zz), str(X)); os.makedirs(d, exist_ok=True)
            p = os.path.join(d, f'{Y}.jpg'); tile.save(p, quality=85); n += 1; nbytes += os.path.getsize(p)
sets = ''.join(f'<TileSet href="{zz}" units-per-pixel="{180.0/256/(1<<zz):.16g}" order="{zz}"/>' for zz in range(a.minz, a.maxz + 1))
open(os.path.join(a.out, 'tilemapresource.xml'), 'w').write(f'''<?xml version="1.0" encoding="utf-8"?>
<TileMap version="1.0.0" tilemapservice="http://tms.osgeo.org/1.0.0"><Title>NAIP {a.year} spike</Title><Abstract/><SRS>EPSG:4326</SRS>
<BoundingBox minx="{W+dlo:.8f}" miny="{S+dla:.8f}" maxx="{E+dlo:.8f}" maxy="{N+dla:.8f}"/><Origin x="-180" y="-90"/>
<TileFormat width="256" height="256" mime-type="image/jpeg" extension="jpg"/><TileSets profile="geodetic">{sets}</TileSets></TileMap>
''')
print(f'{n} tiles {nbytes/1e6:.1f} MB')
