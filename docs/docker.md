# Running CamSim in Docker (NVIDIA GPU)

The image holds the packaged game plus a thin Ubuntu 24.04 runtime. The NVIDIA
driver is not in the image: the NVIDIA Container Toolkit mounts the host's
driver libraries in at run time, so one image works with any recent driver.
The GPU does both jobs it does natively: Vulkan rendering (the GPU sensor
graph has no CPU fallback) and NVENC H.264/H.265 encoding.

## Host requirements

- Linux with an NVIDIA GPU and driver (verified: RTX 5080, driver 595.91).
- Docker 20.10+ and the [NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html)
  (verified: 1.20.1). Check with
  `docker run --rm --gpus all ubuntu:24.04 nvidia-smi`.
- Outbound internet for Cesium tiles, and `CAMSIM_CESIUM_ION_TOKEN` unless the
  level's default token suffices.
- `net.core.rmem_max` raised on receivers (see CLAUDE.md, Linux UDP receive buffers).

Docker Desktop (macOS/Windows) has no NVIDIA GPU passthrough; run natively there.

**A GPU is required.** There is no CPU sensor path, and Mesa lavapipe (CPU Vulkan)
can't run UE 5.8: it segfaults compiling the engine's SM6 pipelines (Mesa 25.2.8
and 26.2.3, ROADMAP 1.15). Without a GPU the entrypoint exits with an error;
`CAMSIM_ALLOW_SOFTWARE_RENDERING=1` makes it try lavapipe anyway (it passes
`-AllowSoftwareRendering` and shrinks Nanite's streaming pool under lavapipe's
128 MB allocation limit), for testing newer Mesa.

## Build

```bash
scripts/package_for_docker.sh            # BuildCookRun -> deploy/staged/Linux/ (+ entities/)
docker build -t camsim:latest deploy/    # or: docker compose -f deploy/docker-compose.yml build
```

The package is a Development build (logging and console intact). The editor
target must build first (`scripts/run.sh --build-only`), since the cook runs the editor.

## Run

```bash
docker compose -f deploy/docker-compose.yml up
# or
docker run --rm --gpus all --init --network host --shm-size 1g \
  -e CAMSIM_CESIUM_ION_TOKEN=... camsim:latest
```

- `--gpus all` (or compose's `deploy.resources.reservations.devices`) passes
  the GPU in. The image sets `NVIDIA_DRIVER_CAPABILITIES=all`: `graphics` is
  needed for Vulkan and `video` for NVENC. Don't override it with
  `compute,utility`.
- `--network host` is required for multicast output and CIGI input.
- Configure with `CAMSIM_*` variables ([configuration.md](configuration.md)),
  or mount a config over `/opt/camsim/CamSimTest/camsim_config.yaml`.
- Extra arguments after the image name go to UE.
- On hybrid Intel CPUs (P- and E-cores, e.g. Core Ultra 285K) the entrypoint pins
  CamSim to the P-cores (`/sys/devices/cpu_core/cpus`, via `taskset`). Unpinned, the
  game thread sometimes spends a whole run on an E-core and runs ~45% slower
  (500 entities: game p50 9.4 vs 6.5 ms); pinned, runs repeat within 0.1 ms and the
  render thread is ~10% faster too. `CAMSIM_PIN_PCORES=0` disables it; it is skipped
  when the container's cpuset excludes the P-cores.
- Health: `GET :8080/live` (the image `HEALTHCHECK`), `/ready` (needs CIGI
  traffic), `/metrics`.
- State lives in two volumes (compose creates them): `/var/lib/camsim` is
  UE's user dir (the entrypoint passes `-userdir`), holding `Saved/` (logs at
  `Saved/Logs/CamSimTest.log`) and Cesium's tile cache database;
  `/home/camsim/.cache` holds the NVIDIA shader cache. Measured on an RTX 5080:
  `/ready` 26–30 s from a cold start, 18 s warm.
- On SIGTERM UE shuts down cleanly in ~10 s and exits 143 (128 + SIGTERM, UE's
  convention). Compose allows 30 s (`stop_grace_period`); with plain
  `docker stop`, pass `-t 30`.

### Several instances on one host

Each instance needs its own CIGI, response, health and stream ports and its own cache volume. How many fit is set by GPU memory
(about 3.3 GB per 720p instance): on the RTX 5080 development box, 4 run comfortably, 5 at the edge, 6 fail. Measurements, a
`docker run` template and the probe (`scripts/bench/scale_bench.py`): [`capacity-linux-rtx5080.md`](capacity-linux-rtx5080.md).

## Validate

```bash
scripts/ci_validate.sh --docker camsim:latest
```

Runs the image with `--gpus all`, drives it with a scripted CIGI host, waits
for `/ready`, then checks the H.264 stream decodes and the KLV conforms to
misb.js with the commanded sensor position. CI's `integration-test` job does
the same on the self-hosted runner.

## How the GPU gets in, and what breaks it

| Symptom | Cause |
|---|---|
| `vkCreateInstance failed with ERROR_INCOMPATIBLE_DRIVER` | The NVIDIA Vulkan ICD (`libGLX_nvidia.so.0`) dlopens `libEGL.so.1`, a distro package (`libegl1`) the toolkit does not inject. The image installs it. |
| Entrypoint: "device node present but Vulkan found no device" | `NVIDIA_DRIVER_CAPABILITIES` lacks `graphics`. |
| Entrypoint: "No NVIDIA GPU passed in", exit 1 | No `--gpus` / device reservation (see "A GPU is required"). |
| Encoder falls back to libx264 | `NVIDIA_DRIVER_CAPABILITIES` lacks `video` (no `libnvidia-encode`). |

Notes:

- With CDI injection (toolkit 1.17+), `NVIDIA_VISIBLE_DEVICES` reads `void`
  inside the container even when the GPU is present, so the entrypoint
  detects the GPU by `/dev/nvidia*` instead.
- The toolkit puts the ICD manifest at `/etc/vulkan/icd.d/nvidia_icd.json`;
  the entrypoint leaves `VK_ICD_FILENAMES` unset so the loader finds it.
- Don't install `libnvidia-*` packages in the image: they would shadow or
  conflict with the host driver's libraries.
