# Multi-instance capacity: Linux / RTX 5080 box

How many CamSim instances one machine runs at once, measured 2026-10-03 on the Linux development box. Short
answer: **4 comfortably, 5 at the edge, 6 fails.** The limit is GPU memory, not CPU, system RAM or the encoder.

## The machine

| | |
|---|---|
| CPU | Intel Core Ultra 9 285K: 24 cores (8 P-cores 0-7, 16 E-cores 8-23), 1 thread per core, up to 5.7 GHz |
| RAM | 62 GiB |
| GPU | NVIDIA GeForce RTX 5080, 16 GB (16303 MiB), PCIe Gen 5 x16 |
| Driver | NVIDIA 595.91.07 (open kernel module) |
| OS | Ubuntu 24.04.5 LTS, kernel 6.17.0-1032-oem |
| Containers | Docker 29.8.2, NVIDIA Container Toolkit 1.20.1 |
| Disk | 848 GB root volume |

## What was measured

- **Build:** CamSim at git `6777e71` (ROADMAP 4C merged), packaged with `scripts/package_for_docker.sh` into the Docker image
  (`docs/docker.md`), one container per instance, `--network host`, `--gpus all`.
- **Per instance:** 1280x720 at 30 fps (the shipped `camsim_config.yaml`), H.264 with NVENC, unicast stream to 127.0.0.1, DIS off,
  its own CIGI / response / health / stream ports and its own Cesium tile cache volume.
- **Scene:** each instance is driven by its own 30 Hz CIGI host flying the benchmark orbit (San Francisco, 3 km up, 2 km radius,
  gimbal 35 deg down; `scripts/bench/scenario.py`), phases staggered 13 s apart, so every instance streams Cesium World Terrain and
  imagery while moving. EO unless marked IR (MWIR preset, thermal radiance pass on).
- **Window:** after every instance reported ready (terrain gate open) and 45 s of warm-up, 90 s of frame stats per instance
  (`CAMSIM_FRAME_STATS_PATH`) and 1 Hz `nvidia-smi` samples.

## Results

| Instances | Worst instance fps | Frame time p95 / p99 (worst) | Frames > 40 ms | GPU busy (p50 / p95) | VRAM used | CPU (whole machine) |
|---|---|---|---|---|---|---|
| 1 | 30.0 | 33.6 / 34.3 ms | 0 % | 9 / 10 % | 3.4 GB | 2 % |
| 2 | 30.0 | 33.6 / 34.9 ms | 0 % | 18 / 19 % | 6.6 GB | 4 % |
| 3 | 30.0 | 33.6 / 34.5 ms | 0.04 % | 27 / 31 % | 9.9 GB | 7 % |
| **4** | **30.0** | **33.8 / 35.0 ms** | **0.04 %** | **42 / 46 %** | **13.1 GB** | **9 %** |
| 4, IR | 30.0 | 33.6 / 34.9 ms | 0.07 % | 38 / 40 % | 14.1 GB | 9 % |
| 5 | 30.0 | 33.7 / 35.2 ms | 0.04 % | 62 / 70 % | 15.8 GB (97 %) | 11 % |
| 5, IR | 30.0 | 33.9 / 35.3 ms | 0.07 % | 52 / 57 % | 15.7 GB | 11 % |
| 6 | **21.3** | **40.0 / 42.6 ms** | **4.9 %** | **96 / 97 %** | 15.8 GB, full | 14 % |

No frames were dropped in any run (the encoder never fell behind) except as below at 6. Every instance used `h264_nvenc`.
The NVENC encoder itself stayed at 1-6 % busy; each container used 1.2-1.8 GB of system RAM and about 0.5-0.6 of a CPU core.

**What happens past the limit.**

- **5 instances:** steady state is fine, but VRAM is at 97 %. In the IR run one container's NVENC failed to create its CUDA context
  at startup (`CUDA_ERROR_OUT_OF_MEMORY`) and recovered; a view that streams more tiles, or a sixth process on the GPU, would tip it over.
- **6 instances:** VRAM runs out. NVENC encodes fail (`EncodePicture failed!: out of memory`), the GPU time-slices between the
  processes (per-instance GPU time goes from ~3 ms to ~31 ms per frame), and one instance delivers 21 fps instead of 30.

## Guidance

- **Run up to 4 instances** on this box at 720p30, EO or IR. That leaves ~2-3 GB of VRAM and more than half the GPU free.
- **VRAM is the budget: ~3.3 GB per instance** at 720p. CPU (9 % of the machine at 4 instances), system RAM (62 GiB) and NVENC are
  far from their limits. GeForce drivers also cap concurrent NVENC sessions (8 on this driver), which only matters past 8 instances.
- **More instances need less VRAM per instance.** The GPU is only ~45 % busy at 4, so compute is not the next wall. Each instance
  sets a 1000 MB texture streaming pool (`r.Streaming.PoolSize`) and Nanite reserves a 512 MB streaming pool; shrinking those could
  fit 5-6 comfortably at some cost in texture detail. Untested.
- **Higher resolution costs VRAM and GPU per instance:** 1080p (render targets, TSR history, readback ring) will lower the count;
  re-measure before relying on it.
- **Use the Docker image, not the editor build** (`scripts/run.sh`), for several instances: the editor build carries uncooked
  assets (~4.9 GB RSS vs ~2.8 GB, ROADMAP 1.15) and `run.sh` keeps one PID file.
- **CPU pinning:** every container pins itself to the 8 P-cores by default (`CAMSIM_PIN_PCORES`, `deploy/entrypoint.sh`). With 6
  instances on those cores the CPU was still not the limit; at higher counts consider splitting containers across P- and E-cores.

## Running several instances

Give each instance its own ports and cache volume (instance `i`):

```bash
docker run -d --name camsim-$i --init --network host --gpus all --shm-size 1g \
  -e CAMSIM_CIGI_PORT=$((9000 + 10*i)) -e CAMSIM_CIGI_RESPONSE_PORT=$((9001 + 10*i)) \
  -e CAMSIM_HEALTH_HTTP_PORT=$((18080 + i)) \
  -e CAMSIM_MULTICAST_ADDR=239.1.1.1 -e CAMSIM_MULTICAST_PORT=$((6000 + 2*i)) \
  -v camsim-data-$i:/var/lib/camsim \
  camsim:latest
```

Each host then sends CIGI to its instance's port and reads `/ready` on its health port. Separate cache volumes avoid several
processes sharing one Cesium tile database.

## Reproducing

```bash
scripts/package_for_docker.sh && docker build -t camsim:latest deploy/
uv run --with numpy scripts/bench/scale_bench.py camsim:latest .cache/scale 1 2 3 4 5 6
uv run --with numpy scripts/bench/scale_bench.py camsim:latest .cache/scale 4 5 --ir
```

One line per instance count lands in `.cache/scale/summary.txt`; `n<N>/result.json` has per-instance numbers and the container
logs sit next to it. A full sweep takes about 30 minutes; the first run per volume also fills the Cesium tile caches.
