# Linux box tasks

A hand-off for an agent working on the Linux reference box. Everything here is still open there; the Mac
(M1 Pro / Metal) is the day-to-day platform and has already done its share. Written 2026-10-10 at commit
`1c028f2`.

## Before you start

- Read `CLAUDE.md` first (gotchas, commands, test invocations). Background for task 1:
  `docs/scene-packages.md` ("Packing and mounting", "Using a package in CamSim", "Measured"); for tasks 2–4:
  `docs/docker.md` and `docs/ci-runner-setup.md`.
- The box: Ubuntu 24.04, Core Ultra 9 285K (24 cores, 62 GB), RTX 5080, driver 595.91, Vulkan SM6, NVENC,
  UE 5.8.3 at `/opt/UnrealEngine-5.8.3` (`/opt/UE` symlink), Cesium in the engine, Docker + NVIDIA Container
  Toolkit. Check with `nvidia-smi`, `/opt/UE/Engine/Binaries/Linux/UnrealEditor -version` and
  `docker run --rm --gpus all ubuntu:24.04 nvidia-smi`; if anything is missing, stop and tell the human.
- Work on a branch (`linux/verify-2026-10`), one commit per task. Don't push or open a PR unless asked.
- Ask the human before anything that needs `sudo`, and before changes to GitHub settings (task 4).
- Report results where the project records them, in its style: the `ROADMAP.md` entry and the doc named in each
  task, with the date, hardware and numbers, and mark the item done (`~~…~~ **Done YYYY-MM-DD**`) or say what's
  still open. A fail is a result too: record it and explain it. Don't loosen a gate to make it pass.
- Run `run.sh` in the foreground or with `set -o pipefail` (never piped through a bare `tee`).

## 0. Sync and regression check (do this first)

Everything since 2026-10-07 (R1: seabed, `sea_level.json`, the ocean datum offset in `FOceanSurface`, bench
`--site pendleton`) was verified on Metal only.

```bash
git pull && git lfs pull
scripts/build_thirdparty.sh            # no-op when cached
scripts/run.sh --build-only
"$UE_BIN" unreal_project/CamSimTest/CamSimTest.uproject \
  -ExecCmds="Automation RunTests CamSim+Quit" -TestExit="Automation Test Queue Empty" \
  -ReportExportPath=.cache/automation-report -unattended -nullrhi -nosound -nosplash -log -stdout -FullStdOutLogOutput
python3 scripts/parse_automation_report.py .cache/automation-report/index.json
scripts/run_gpu_tests.sh               # all CamSim.GPU.* on Vulkan
scripts/ci_validate.sh --native
uv run --project scripts/scene --with pytest pytest scripts/scene/tests
```

Then `scripts/thermal_check.py --band both` and `scripts/ocean_check.py` (the ocean code changed). Record the counts
in `ROADMAP.md` under "R1 completed on macOS". Fix regressions before going on, or stop and report them.

`CamSim.GPU.GroundTruth.*` covers `InstanceIdCS` on Vulkan (`ROADMAP.md` 2.7's "Linux/Vulkan unverified" entry was
struck through on 2026-10-10 from the 2026-10-01 run); if it fails now, reopen that entry.

## 1. Realism R1: the Linux items (main task)

Open in `ROADMAP.md` (Realism track, "R1 completed on macOS 2026-10-10 … Open on Linux") and
`docs/scene-packages.md` ("R1 completion", "Still open"). Three checks: CamSim reading a package by `file://` from a
mounted volume in Docker, an offline run with no network, and cold-start timing from SquashFS vs a plain directory.
This also closes the R0 gate-5 note "Repeating the offline run on Linux/Vulkan needs an egress-blocking equivalent".

### 1a. The package

The accepted R1 package (`pendleton-r1c`) was built on the Mac; the Linux box has earlier packages under
`.cache/scene-packages/` (R0 `pendleton`, `pendleton-r1`, `pendleton-r1b`) and a fetch cache in `.cache/scene`.
Build the current one in the reference container from a copy (`cp -a`; keep the originals):

```bash
cp -a .cache/scene-packages/pendleton-r1b .cache/scene-packages/pendleton-r1c
scripts/scene/build.sh --build-image build /out/pendleton-r1c \
  --config scripts/scene/examples/pendleton.toml --replan -j 16
scripts/scene/build.sh verify /out/pendleton-r1c --deep -j 16
scripts/scene/build.sh pack /out/pendleton-r1c
```

`--replan` adds the NDVI layer, seabed terrain and `sea_level.json` and skips unchanged tiles. Expect the terrain to
grow to 182,794 tiles, NDVI 7,043, imagery 277,286 and land cover 2,288; `sea_level.json` offset +0.110 m (NOAA
9410230 La Jolla). Record wall time and peak RSS. A `-j 1` build reports a false RSS failure (known follow-up);
use `-j 16`.

### 1b. The image

```bash
scripts/package_for_docker.sh
docker build -t camsim:linux-r1 deploy/
scripts/ci_validate.sh --docker camsim:linux-r1          # must pass before the scene runs
```

Use CDI (`--device nvidia.com/gpu=all`) if `--gpus all` leaves Vulkan on lavapipe (`docs/docker.md`).

### 1c. Mount and run (`file://` from a mounted volume)

```bash
sudo mkdir -p /mnt/pendleton && sudo mount -o loop,ro .cache/scene-packages/pendleton-r1c.sqfs /mnt/pendleton
# or, without root: squashfuse .cache/scene-packages/pendleton-r1c.sqfs /mnt/pendleton
docker run ... -v /mnt/pendleton:/data/scene/pendleton:ro \
  -e CAMSIM_SCENE_DIR=/data/scene/pendleton -e CAMSIM_SCENE_OFFLINE=1 -e CAMSIM_SNAPSHOT_ENDPOINT_ENABLED=1 ...
```

**Tooling gap:** `scripts/scene/tools/render_check.py` and `camsim_session.py` only launch the native build through
`run.sh`, read the macOS log path (`~/Library/Logs/CamSimTest/`), and use macOS `sandbox-exec` for `--offline`. Add a
`--docker IMAGE` mode, reusing `run_bench.py`'s `docker_run_cmd` / `docker_alive` / `docker logs`, that mounts the
package read-only and sets the env above. Read the log with `docker logs` (or the `/var/lib/camsim` volume:
`Saved/Logs/CamSimTest.log`), and fix the native log path on Linux as well
(`unreal_project/CamSimTest/Saved/Logs/CamSimTest.log`). Keep the macOS path working. `hot_check.py` uses the same
session helper and should get the same mode.

Gates (as on the Mac, `docs/scene-packages.md` "R1 completion"): every `render_check.py` shot `terrain_ready`, no
"terrain gate timed out" line, the shots look right (send the human a few: `nadir_2km`, `coast_low`, `coast_waves`,
`slant_ne_ir`), and `hot_check.py` 7/7 land + 3/3 sea (sea HOT = EGM96 + 0.110 m). Registration vs the Mac's shots
is not a gate (different GPU); vs a Linux `cwt` run it is (≤ 0.16 px on the Mac, < 1 px limit).

### 1d. Offline (no egress)

The aim: CamSim on the package makes no network request and still renders. `docker run --network none` is the
literal check but leaves no way in for the CIGI host or `/ready`. Either:

- run the CIGI host inside the container's namespace (`docker run --network container:<camsim>` for a small host
  container, or `docker exec` a Python host in the CamSim container), with CamSim on `--network none`; or
- put CamSim on an internal network (`docker network create --internal camsim-offline`), which has no route out but
  is reachable from the host at the container's IP; point the CIGI host and health checks at that IP.

Prefer the literal `--network none`; if you use the internal network, say so in the results. Pass: every shot
ready, and no `LogHttp:` or `cesium.com` line in the log (the `render_check.py` `network_lines` test), and
`scene.offline` didn't exit with status 1. As a negative check, run once with `CAMSIM_SCENE_DIR` unset under the
same isolation and confirm it fails or logs blocked requests (so the test can fail).

### 1e. Cold start: SquashFS vs plain directory

Same image, same shots, the package from the loop-mounted `.sqfs` vs the unpacked directory bind-mounted `:ro`.
Measure each cold (drop the page cache between runs: `sync; echo 3 | sudo tee /proc/sys/vm/drop_caches`, ask
first) and warm, 3 runs each: time to `/ready` and `render_check.py`'s per-shot `ready_s`. Background and
expectations: `docs/realism/offline-hosting.md`. The Mac's numbers (plain directory, native): first ready 17 s
package, 32 s offline.

### 1f. Optional: bench

`scripts/bench/run_bench.py --docker camsim:linux-r1 --site pendleton --label linux-r1c` with the package mounted
(add the mount through `--env`/a small change to `docker_run_cmd`), 2 runs, vs the same on Cesium ion. Pass as on the
Mac: 30 fps, 0 dropped frames, game-thread p50 of the package ≤ ion's in every phase.

Record 1a–1f in `docs/scene-packages.md` (a "R1 on Linux" table after "R1 completion", and replace its "Still
open" line) and in `ROADMAP.md`'s Realism paragraph.

## 2. Host setting: UDP receive buffers

ROADMAP 1.14 / the 2026-09-30 follow-up: persist `net.core.rmem_max` on this host (needs sudo, ask first):

```bash
echo 'net.core.rmem_max=26214400' | sudo tee /etc/sysctl.d/90-camsim.conf && sudo sysctl --system
```

Record that it's done in the "Human follow-ups" list in `ROADMAP.md` (Milestone 0 results).

## 3. Undiagnosed: the Linux Shipping package never became ready

ROADMAP 1.15: a Linux **Shipping** package launched by `run.sh` sat at `first_frame: false, terrain_ready: false`
for 12 min with CIGI flowing. Shipping writes no log, so the cause is unknown; the Development package in Docker
works. Use `superpowers:systematic-debugging`. Leads: build Shipping with logging (`bUseLoggingInShipping = true`
in the Game target, or `-log` with a `-forcelogflush` build), or compare which assets/config a Shipping cook
drops (`DirectoriesToAlwaysCook`, `camsim_config.yaml` lookup, Cesium ion token, `NonUFS` data). Stage under
`.cache/` (never `Saved/StagedBuilds/`: `run.sh` then switches to packaged mode, `CLAUDE.md` Docker section). Fix it
or record what you found under 1.15.

## 4. CI runner (deferred by the user: ask before starting)

`docs/ci-runner-setup.md`. The box isn't registered as the `camsim-ue5` runner, so the `unit-tests`,
`integration-test`, `docker-build` and `docker-release` jobs have never run. Registration needs a token from
the GitHub UI and flipping `CAMSIM_UE5_RUNNER_AVAILABLE`, which are the human's call.

Can be done without the runner:

- **Packaging uses the prebuilt Cesium** (open since Milestone 0): in the `package_for_docker.sh` output, check
  that UBT links `CesiumRuntime` from the engine's `Marketplace` plugin binaries and compiles no Cesium source
  (`grep -i cesium` the UBT log for `.cpp`). Record it in the Milestone 0 "Human follow-ups".
- **Self-contained NullRHI tests** (`ROADMAP.md` 3B.2 findings, "Before Linux CI"): the tests read the gitignored
  `unreal_project/CamSimTest/camsim_config.yaml`, which `run.sh` refreshes and a direct test run doesn't. Make them
  independent of it (a clean checkout must pass), and add the two GPU tests listed there (NaN bloom texel;
  non-same-size + bloom + blur partial thread groups). This is code work (TDD, both platforms), so ask before
  starting it.

## Not for this box

- RTX 5090 reference benchmarks (3A/3B): deferred until that hardware exists.
- HITL rig checks (`hitl/README.md`): need the X-Plane/PX4 rig.
- 4C entity thermal on Metal: that's the Mac's.
