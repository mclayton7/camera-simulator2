"""Spike: quantized-mesh terrain for a small area from 3DEP (1/3 arc-second + 1 m where present).
Heights: NAVD88 -> ITRF2014 ellipsoid (GEOID18), horizontal NAD83(2011) -> ITRF2014 as a constant shift.
Outside the source: flat sea floor (EGM96 geoid - 15 m), so the globe is covered offline."""
import argparse, gzip, json, math, os, sys, time
import numpy as np, rasterio, pyproj
from rasterio.warp import reproject, Resampling
from rasterio.transform import from_origin
from scipy.ndimage import map_coordinates
from pydelatin import Delatin
from pydelatin.util import rescale_positions
import quantized_mesh_encoder as qme
from quantized_mesh_encoder.extensions import VertexNormalsExtension
from quantized_mesh_encoder.ecef import to_ecef
from quantized_mesh_encoder.normals import compute_vertex_normals, oct_encode
from struct import pack

import quantized_mesh_encoder.encode as _E
_POS64 = {}
def _interp64(positions, bounds=None):
    # the encoder casts positions to float32 (~0.7 m at lon -117) and truncates; quantize from float64 and round
    p = _POS64['p']; minx, miny, maxx, maxy = bounds
    minh, maxh = p[:, 2].min(), p[:, 2].max()
    q = lambda a, lo, hi: np.rint((a - lo) / (hi - lo) * 32767 if hi > lo else a * 0).clip(0, 32767).astype(np.int16)
    return np.vstack([q(p[:, 0], minx, maxx), q(p[:, 1], miny, maxy), q(p[:, 2], minh, maxh)]).T
_E.interp_positions = _interp64

class SafeNormals(VertexNormalsExtension):
    # degenerate triangles (tile corners at the poles) give NaN normals: use the ellipsoid up vector there
    def encode(self):
        p = self.positions.reshape(-1, 3); c = to_ecef(p, ellipsoid=self.ellipsoid)
        n = compute_vertex_normals(c, self.indices)
        lon = np.radians(p[:, 0]); lat = np.radians(p[:, 1])
        up = np.stack([np.cos(lat) * np.cos(lon), np.cos(lat) * np.sin(lon), np.sin(lat)], 1)
        bad = ~np.isfinite(n).all(1); n[bad] = up[bad]
        enc = oct_encode(n).tobytes('C')
        return pack('<B', self.id.value) + pack('<I', len(enc)) + enc

ap = argparse.ArgumentParser()
ap.add_argument('--bbox', type=float, nargs=4, required=True)  # W S E N
ap.add_argument('--third', required=True)       # 1/3 arc-second COG url
ap.add_argument('--onem', nargs='*', default=[]) # 1 m COG urls
ap.add_argument('--res', type=float, default=1.0)
ap.add_argument('--maxz', type=int, default=16)
ap.add_argument('--out', required=True)
ap.add_argument('--grid', type=int, default=257)
a = ap.parse_args()
pyproj.network.set_network_enabled(True)
W, S, E, N = a.bbox
lat0 = (S + N) / 2
dlat = a.res / 110950.0; dlon = a.res / (111320.0 * math.cos(math.radians(lat0)))
w = int((E - W) / dlon); h = int((N - S) / dlat)
T = from_origin(W, N, dlon, dlat)
env = dict(GDAL_DISABLE_READDIR_ON_OPEN='EMPTY_DIR', CPL_VSIL_CURL_ALLOWED_EXTENSIONS='.tif')
def warp(url, resamp):
    out = np.full((h, w), np.nan, np.float32)
    with rasterio.Env(**env), rasterio.open(('/vsicurl/' + url) if url.startswith('http') else url) as ds:
        reproject(rasterio.band(ds, 1), out, dst_transform=T, dst_crs='EPSG:6318', dst_nodata=np.nan, resampling=resamp, src_nodata=ds.nodata)
    return out
t0 = time.time()
dem = warp(a.third, Resampling.bilinear); src = np.zeros((h, w), np.uint8); src[np.isfinite(dem)] = 1
for u in a.onem:
    m = warp(u, Resampling.bilinear); v = np.isfinite(m); dem[v] = m[v]; src[v] = 2
print(f'mosaic {w}x{h} in {time.time()-t0:.0f}s; 1 m share {np.mean(src==2):.1%}, 1/3" {np.mean(src==1):.1%}, none {np.mean(src==0):.1%}')
# vertical: NAVD88 -> ellipsoid offset on a coarse grid; horizontal shift at the centre
tr = pyproj.Transformer.from_crs('EPSG:6318+5703', 'EPSG:7912', always_xy=True)
cl = np.linspace(W, E, 9); cb = np.linspace(S, N, 9); LL, BB = np.meshgrid(cl, cb)
lon2, lat2, hh = tr.transform(LL.ravel(), BB.ravel(), np.zeros(LL.size))
off = hh.reshape(LL.shape)
dlon_shift = float(np.mean(lon2 - LL.ravel())); dlat_shift = float(np.mean(lat2 - BB.ravel()))
egm = pyproj.Transformer.from_crs('EPSG:4979', 'EPSG:4326+5773', always_xy=True)
geoid_n = float(0 - egm.transform((W+E)/2, (S+N)/2, 0)[2])
print(f'vertical offset {off.min():.2f}..{off.max():.2f} m; horizontal shift {dlon_shift*111320*math.cos(math.radians(lat0)):.2f} m E {dlat_shift*110950:.2f} m N; EGM96 N {geoid_n:.2f}')
# apply vertical offset (bilinear over the 9x9 grid)
yy, xx = np.mgrid[0:h, 0:w]
gx = xx / (w - 1) * 8; gy = (h - 1 - yy) / (h - 1) * 8
dem_e = dem + map_coordinates(off, [gy, gx], order=1).astype(np.float32)
fill = geoid_n - 15.0
dem_e[~np.isfinite(dem_e)] = fill
# georeferencing of the ellipsoid-height grid in ITRF lon/lat
W2, N2 = W + dlon_shift, N + dlat_shift
def sample(lon, lat):
    c = (lon - W2) / dlon - 0.5; r = (N2 - lat) / dlat - 0.5
    inside = (c >= 0) & (c <= w - 1) & (r >= 0) & (r <= h - 1)
    out = np.full(lon.shape, fill, np.float32)
    out[inside] = map_coordinates(dem_e, [r[inside], c[inside]], order=1)
    return out
def tile_bounds(z, x, y):
    s = 180.0 / (1 << z)
    return (-180 + x * s, -90 + y * s, -180 + (x + 1) * s, -90 + (y + 1) * s)
os.makedirs(a.out, exist_ok=True)
avail = []; n_tiles = 0; nbytes = 0; t0 = time.time()
for z in range(0, a.maxz + 1):
    s = 180.0 / (1 << z)
    if z == 0: tiles = [(0, 0), (1, 0)]
    else:
        x0 = int((W2 + 180) // s); x1 = int((E + dlon_shift + 180) // s)
        y0 = int((S + dlat_shift + 90) // s); y1 = int((N2 + 90) // s)
        tiles = [(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)]
    g = a.grid
    me = max(0.25, min(50.0, 0.5 * 2 ** (a.maxz - z)))
    for x, y in tiles:
        b = tile_bounds(z, x, y)
        lons = np.linspace(b[0], b[2], g); lats = np.linspace(b[3], b[1], g)
        LO, LA = np.meshgrid(lons, lats)
        arr = sample(LO, LA) if z > 3 else np.full(LO.shape, fill, np.float32)
        tin = Delatin(arr, width=g, height=g, max_error=me)
        verts, tris = tin.vertices, tin.triangles  # pydelatin: y counts up from the last row (south), triangles CCW
        v = verts.astype(np.float64)  # rescale in float64 (pydelatin's helper rounds lon/lat to float32, ~0.7 m)
        pos = np.stack([b[0] + v[:, 0] * (b[2] - b[0]) / (g - 1), b[1] + v[:, 1] * (b[3] - b[1]) / (g - 1), v[:, 2]], 1)
        # snap edges exactly: the encoder truncates to int16, so a hair under the bound misses the edge list (no skirt)
        pos[v[:, 0] == 0, 0] = b[0]; pos[v[:, 0] == g - 1, 0] = b[2]
        pos[v[:, 1] == 0, 1] = b[1]; pos[v[:, 1] == g - 1, 1] = b[3]
        # high-water-mark order: the encoder assumes vertex i first appears after vertices 0..i-1 (it does not reorder)
        flat = tris.ravel(); _, first = np.unique(flat, return_index=True)
        order = flat[np.sort(first)]; remap = np.empty(len(pos), np.int64); remap[order] = np.arange(len(order))
        pos = pos[order]; tris = remap[tris].astype(np.uint32)
        d = os.path.join(a.out, str(z), str(x)); os.makedirs(d, exist_ok=True)
        with gzip.open(os.path.join(d, f'{y}.terrain'), 'wb') as f:
            _POS64['p'] = pos
            qme.encode(f, pos, tris, bounds=b, extensions=[SafeNormals(indices=tris, positions=pos)])
        nbytes += os.path.getsize(os.path.join(d, f'{y}.terrain')); n_tiles += 1
    ranges = []
    for y in sorted({t[1] for t in tiles}):
        xs = sorted(t[0] for t in tiles if t[1] == y)
        ranges.append({'startX': xs[0], 'startY': y, 'endX': xs[-1], 'endY': y})
    avail.append(ranges)
    print(f'z{z}: {len(tiles)} tiles, max_error {me} m', flush=True)
layer = {'tilejson': '2.1.0', 'name': 'camsim-spike', 'format': 'quantized-mesh-1.0', 'version': '1.0.0', 'scheme': 'tms',
         'tiles': ['{z}/{x}/{y}.terrain'], 'projection': 'EPSG:4326', 'bounds': [-180, -90, 180, 90],
         'minzoom': 0, 'maxzoom': a.maxz, 'extensions': ['octvertexnormals'], 'available': avail}
json.dump(layer, open(os.path.join(a.out, 'layer.json'), 'w'))
print(f'{n_tiles} tiles, {nbytes/1e6:.1f} MB gz, {time.time()-t0:.0f}s')
# verify: decode tiles and compare vertex heights with the source at the decoded lon/lat
sys.path.insert(0, os.path.dirname(__file__)); from qm_decode import decode
import random
random.seed(1)
for z in (13, 14, 15, 16):
    errs = []
    for x in os.listdir(os.path.join(a.out, str(z))):
        for f in os.listdir(os.path.join(a.out, str(z), x)):
            y = int(f.split('.')[0]); b = tile_bounds(z, int(x), y)
            u, v, hq = decode(os.path.join(a.out, str(z), x, f))
            lon = b[0] + u / 32767 * (b[2] - b[0]); lat = b[1] + v / 32767 * (b[3] - b[1])
            m = (lon > W2 + 0.001) & (lon < E + dlon_shift - 0.001) & (lat > S + dlat_shift + 0.001) & (lat < N2 - 0.001)
            if m.any(): errs.append(np.abs(sample(lon[m], lat[m]) - hq[m]))
    e = np.concatenate(errs)
    print(f'verify z{z}: |vertex - source| p50 {np.percentile(e,50):.2f} p99 {np.percentile(e,99):.2f} max {e.max():.2f} m ({len(e)} vertices >100 m inside the bbox)')
