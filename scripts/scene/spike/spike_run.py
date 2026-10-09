"""Spike: launch CamSim with a local scene package (file://), fly shots, save /snapshot PNGs + log excerpts."""
import os, sys, time, subprocess, json
from dataclasses import replace
from pathlib import Path
REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'scripts')); sys.path.insert(0, str(REPO / 'scripts' / 'bench'))
import run_bench as rb
from bench import scenario
out = Path(sys.argv[1]); mode = sys.argv[2]; out.mkdir(parents=True, exist_ok=True)
pkg = Path(sys.argv[3]) if len(sys.argv) > 3 else None
env = {'CAMSIM_SNAPSHOT_ENDPOINT_ENABLED': '1'}
if mode == 'localterrain':
    env.update(CAMSIM_CESIUM_TERRAIN_SOURCE='url', CAMSIM_CESIUM_TERRAIN_URL=f'file://{pkg}/terrain/layer.json')
if mode == 'local':
    env.update(CAMSIM_CESIUM_TERRAIN_SOURCE='url', CAMSIM_CESIUM_TERRAIN_URL=f'file://{pkg}/terrain/layer.json',
               CAMSIM_CESIUM_IMAGERY_SOURCE='tms', CAMSIM_CESIUM_IMAGERY_URL=f'file://{pkg}/imagery/tilemapresource.xml',
               CAMSIM_CESIUM_ION_TOKEN='')
P = scenario.Pose
shots = {
  'nadir_2km':    P(lat=33.225, lon=-117.380, alt=2000, gimbal_pitch=-90, fov_h=30),
  'slant_ne':     P(lat=33.205, lon=-117.405, alt=800, yaw=45, gimbal_pitch=-20, fov_h=40),
  'rivermouth':   P(lat=33.222, lon=-117.400, alt=250, yaw=90, gimbal_pitch=-25, fov_h=50),
  'high_edge':    P(lat=33.225, lon=-117.380, alt=6000, yaw=0, gimbal_pitch=-35, fov_h=60),
  'slant_ne_ir':  P(lat=33.205, lon=-117.405, alt=800, yaw=45, gimbal_pitch=-20, fov_h=40, sensor_id=1),
}
host = rb.Host(); host.pose = shots['nadir_2km']; host.thread.start()
subprocess.run([str(REPO/'scripts'/'run.sh'), '--headless', '--local', '--detach'], env=dict(os.environ, **env), check=True, stdout=subprocess.DEVNULL)
pid = REPO/'.cache'/'camsim.pid'; t0 = time.time(); res = {}
try:
    rb.wait_ready(pid); res['ready_s'] = round(time.time()-t0)
    for name, pose in shots.items():
        host.pose = pose; time.sleep(1.0)
        ok = rb.wait_terrain(90); time.sleep(rb.SHOT_SETTLE_S + 2)
        rb.fetch_snapshot(out / f'{name}.png'); res[name] = {'terrain_ready': ok}
        print(name, ok, flush=True)
    res['metrics'] = [l for l in (rb.http_text('/metrics') or '').splitlines() if 'tile' in l.lower() or 'terrain' in l.lower()][:20]
finally:
    subprocess.run([str(REPO/'scripts'/'stop.sh')], check=False, stdout=subprocess.DEVNULL); host.stop.set()
json.dump(res, open(out/'result.json','w'), indent=1); print(json.dumps(res, indent=1))
