"""Frame-centre (CIGI 107) altitude at nadir points vs 3DEP truth (NAVD88 -> ITRF2014 ellipsoid via GEOID18)."""
import os, sys, time, socket, subprocess, json, statistics
from pathlib import Path
import numpy as np, rasterio, pyproj
REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO/'scripts')); sys.path.insert(0, str(REPO/'scripts'/'bench'))
import run_bench as rb, check_cigi_responses as ccr
from bench import scenario
mode, pkg = sys.argv[1], sys.argv[2]
pyproj.network.set_network_enabled(True)
THIRD = sys.argv[3]; ONEM = sys.argv[4]
to_itrf = pyproj.Transformer.from_crs('EPSG:6318+5703', 'EPSG:7912', always_xy=True)
to_nad = pyproj.Transformer.from_crs('EPSG:7912', 'EPSG:6318', always_xy=True)
def truth(lat, lon):
    lo, la = to_nad.transform(lon, lat)
    h = None
    for url in (ONEM, THIRD):
        with rasterio.Env(GDAL_DISABLE_READDIR_ON_OPEN='EMPTY_DIR'), rasterio.open('/vsicurl/' + url) as ds:
            x, y = pyproj.Transformer.from_crs('EPSG:6318', ds.crs, always_xy=True).transform(lo, la)
            v = next(ds.sample([(x, y)]))[0]
            if ds.nodata is None or v != ds.nodata and v > -1000: h, src = float(v), ('1m' if url == ONEM else '1/3"'); break
    return to_itrf.transform(lo, la, h)[2], src
pts = [(33.2100, -117.3700), (33.2300, -117.3600), (33.2400, -117.3900), (33.2150, -117.3950), (33.2450, -117.3700)]
env = {'CAMSIM_SNAPSHOT_ENDPOINT_ENABLED': '1'}
if mode == 'local':
    env.update(CAMSIM_CESIUM_TERRAIN_SOURCE='url', CAMSIM_CESIUM_TERRAIN_URL=f'file://{pkg}/terrain/layer.json',
               CAMSIM_CESIUM_IMAGERY_SOURCE='tms', CAMSIM_CESIUM_IMAGERY_WMS_URL=f'file://{pkg}/imagery/tilemapresource.xml')
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sock.bind(('0.0.0.0', 8889)); sock.settimeout(0.5)
host = rb.Host(); host.pose = scenario.Pose(lat=pts[0][0], lon=pts[0][1], alt=1500, gimbal_pitch=-90, fov_h=20); host.thread.start()
subprocess.run([str(REPO/'scripts'/'run.sh'), '--headless', '--local', '--detach'], env=dict(os.environ, **env), check=True, stdout=subprocess.DEVNULL)
out = []
try:
    rb.wait_ready(REPO/'.cache'/'camsim.pid')
    for lat, lon in pts:
        host.pose = scenario.Pose(lat=lat, lon=lon, alt=1500, gimbal_pitch=-90, fov_h=20)
        time.sleep(1); rb.wait_terrain(90); time.sleep(3)
        got = []; t_end = time.time() + 3
        while time.time() < t_end:
            try: d = sock.recv(65536)
            except socket.timeout: continue
            for s in ccr.parse_responses(d)['sensor_ext']:
                if s['status'] != 0: got.append(s)
        fc = got[-1] if got else None
        t, src = truth(fc['lat'], fc['lon']) if fc else (None, None)
        r = {'pt': (lat, lon), 'fc': fc and (round(fc['lat'], 6), round(fc['lon'], 6), round(fc['alt'], 2)), 'truth_ellipsoid': t and round(t, 2), 'src': src, 'err_m': fc and t and round(fc['alt'] - t, 2)}
        print(r, flush=True); out.append(r)
finally:
    subprocess.run([str(REPO/'scripts'/'stop.sh')], check=False, stdout=subprocess.DEVNULL); host.stop.set()
json.dump(out, open(f'hot_{mode}.json', 'w'), indent=1)
