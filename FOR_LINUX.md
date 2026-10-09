# R0 exit gates on the Linux box

Steps for the REALISM R0 gates that need the reference build container on Linux: gates 1, 2, 3, 4 and 6.
Gate 5 (CamSim render) was run on the Mac on 2026-10-09 (`docs/scene-packages.md`, "Measured"). The gates
are defined in `REALISM.md` (R0) and the tooling is documented in `docs/scene-packages.md`.

When every gate below is done, fill in its row in the "Measured" table in `docs/scene-packages.md`, update the
realism status in `ROADMAP.md`, and delete this file.

## What you need

- Docker (rootless or in the `docker` group). No GPU is needed: these gates only build packages.
- `git`, and `uv` (for the test suite and the helper one-liners below).
- About **90 GB free**: ~35 GB fetch cache, 3 × ~5.4 GB Pendleton packages, 2 × ~5.4 GB `.sqfs` images, plus
  the preview package (`plan` prints its estimate).
- RAM for `-j 16`: terrain workers peaked at ~0.95 GB and imagery at ~0.65 GB on the Mac, so 16 workers need
  ~16 GB. Use a lower `-j` on a smaller machine. Use the **same `-j` for builds A, B and C**.
- Internet for the first build: about 35 GB is downloaded, from USGS (prd-tnm S3), Microsoft Planetary
  Computer (NAIP), NOAA (ETOPO), NASA (Blue Marble) and ESA (WorldCover).

## 0. Set up

```bash
git pull                                    # needs commit 061c837 or later (land-cover RSS fix, verify limits)
uv run --project scripts/scene --with pytest pytest scripts/scene/tests -q    # expect all pass, 1 skipped
scripts/scene/build.sh --build-image --help # builds the reference image camsim-scene:r0 once
docker image inspect --format '{{.Id}}' camsim-scene:r0   # note this digest in the results
```

Build the image **once**. Builds A, B and C must use the same image digest, or the gate 3 comparison doesn't
mean anything. `build.sh` records the digest and the git commit in each package's `build.json`.

Paths: inside the container the repo is `/work`, the fetch cache is `/cache` (host: `.cache/scene`) and outputs
go to `/out` (host: `.cache/scene-packages`). All the commands below run from the repo root.

## Gate 1: Pendleton `sim` build in the reference container

Record wall time, peak RSS per worker, and size and file count per layer.

Fetch first, on its own, so the build's wall time isn't mostly download time:

```bash
set -o pipefail   # so a failing step isn't hidden behind tee's exit code
scripts/scene/build.sh plan /out/pendleton --config scripts/scene/examples/pendleton.toml
time scripts/scene/build.sh fetch /out/pendleton 2>&1 | tee .cache/gate1-fetch.log
time scripts/scene/build.sh build /out/pendleton -j 16 2>&1 | tee .cache/gate1-build.log
```

`plan` prints the estimated cache and package size. This warning is expected: `skipping project
San_Diego_CA_2014_LiDAR: geoid GEOID12A` (GEOID12A is unsupported by design, `docs/scene-packages.md`).

Per-layer numbers come from `build.json`:

```bash
python3 -c "
import json; b = json.load(open('.cache/scene-packages/pendleton/build.json'))
print(b['started_utc'], '->', b['finished_utc'], 'jobs', b['jobs'], 'cpus', b['cpu_count'], 'image', b['image_digest'])
for k, v in b['layers'].items():
    print(f\"{k:9s} tiles {v['tiles']:7d} files {v['files']:7d} {v['bytes']/1e9:6.2f} GB {v['seconds']:7.0f} s peak RSS {v['peak_rss_mb']:5.0f} MB\")"
du -sh .cache/scene-packages/pendleton .cache/scene
```

Pass: the build exits 0. A peak RSS over `--max-worker-rss-mb` (default 2048 MB) makes it exit with an error
after a complete build: record the number either way. For comparison, the native Mac build (2026-10-09, `-j 6`)
took ~20 min of tile work; terrain had 182,602 tiles / 0.98 GB and imagery 253,766 tiles / 1.96 GB.

## Gate 4: `verify --deep`

```bash
scripts/scene/build.sh verify /out/pendleton --deep -j 16
cat .cache/scene-packages/pendleton.verify.json | python3 -m json.tool | head -40
```

Pass: exit 0, every check `ok`. Record the `terrain_heights` line: scene p50 / p99 / max and the vertex count,
plus the base zooms nearest their `p99_limit`. On the Mac the scene was p50 0.001 m and p99 0.049 m, and the
nearest base zooms were z7 (0.84 m of 1.51 m) and z8 (0.42 m of 0.75 m).

Then pack it for gate 3:

```bash
scripts/scene/build.sh pack /out/pendleton
cat .cache/scene-packages/pendleton.sqfs.sha256
```

## Gate 3: a second clean build is byte-identical

Rebuild from build A's `manifest.json` alone, into a new directory. `build` plans only when `manifest.json` is
missing, so a copied manifest is built as-is. The fetch cache is shared, so nothing is downloaded again.

```bash
mkdir -p .cache/scene-packages/pendleton-b
cp .cache/scene-packages/pendleton/manifest.json .cache/scene-packages/pendleton-b/
scripts/scene/build.sh build /out/pendleton-b -j 16
scripts/scene/build.sh pack /out/pendleton-b
cmp .cache/scene-packages/pendleton/hashes.txt .cache/scene-packages/pendleton-b/hashes.txt && echo "hashes.txt identical"
cut -d' ' -f1 .cache/scene-packages/pendleton.sqfs.sha256 .cache/scene-packages/pendleton-b.sqfs.sha256
```

Pass: `hashes.txt` is identical and the two `.sqfs` sha256s are equal. Builds must run on the same machine
(NumPy's `sin`/`cos` kernels depend on CPU features; see "Reproducibility contract" in `docs/scene-packages.md`).

If they differ, list the files that differ before anything else:

```bash
diff <(sort .cache/scene-packages/pendleton/hashes.txt) <(sort .cache/scene-packages/pendleton-b/hashes.txt) | head
```

## Gate 2: a killed build resumes without redoing finished tiles

Run a third build (C) from the same manifest, kill it about halfway through the terrain tiles, then run it again.

```bash
mkdir -p .cache/scene-packages/pendleton-c
cp .cache/scene-packages/pendleton/manifest.json .cache/scene-packages/pendleton-c/
scripts/scene/build.sh build /out/pendleton-c -j 16 2>&1 | tee .cache/gate2-first.log
```

Watch the `terrain: N/182602` progress lines. At about half, kill the container from a second shell:

```bash
docker kill $(docker ps -q --filter ancestor=camsim-scene:r0)
```

Then resume and compare:

```bash
scripts/scene/build.sh build /out/pendleton-c -j 16 2>&1 | tee .cache/gate2-resume.log
grep -E "^(terrain|imagery): [0-9]+/[0-9]+ \([0-9]+ skipped\)" .cache/gate2-resume.log | head -1
python3 -c "
import json; b = json.load(open('.cache/scene-packages/pendleton-c/build.json'))
for k, v in b['layers'].items(): print(k, 'built', v['built'], 'skipped', v['skipped'], 'tiles', v['tiles'])"
cmp .cache/scene-packages/pendleton/hashes.txt .cache/scene-packages/pendleton-c/hashes.txt && echo "resumed build identical to A"
```

Pass: terrain `skipped` is roughly the count it reached before the kill and `built + skipped == tiles`, and the
resumed package's `hashes.txt` matches build A's. The resumed run must start without a lock error: a killed
container releases `.state/lock`.

## Gate 6: a 10,000 km² `preview` build

Record time and size. This uses a different bbox (~100 × 100 km around Pendleton) and the `preview` profile
(no NAIP, no 1 m DEM), so it downloads its own sources.

```bash
scripts/scene/build.sh plan /out/pendleton-preview-10k --config scripts/scene/examples/pendleton-preview-10k.toml
time scripts/scene/build.sh fetch /out/pendleton-preview-10k 2>&1 | tee .cache/gate6-fetch.log
time scripts/scene/build.sh build /out/pendleton-preview-10k -j 16 2>&1 | tee .cache/gate6-build.log
scripts/scene/build.sh verify /out/pendleton-preview-10k --deep -j 16
```

Record `plan`'s estimate next to the real numbers (the `build.json` one-liner from gate 1, with the package path
changed), and `du -sh` of the package. These replace the size estimates in `REALISM.md` section 3 ("Large areas").

## Optional: offline render on Linux

`REALISM.md`'s offline gate is phrased as an egress-blocked container run (`docker run --network none`, unicast
to loopback, CIGI host in the container). It can't pass anywhere yet: `Main.umap`'s Cesium ion actors request
`api.cesium.com` on the first frame (the editor follow-up in `ROADMAP.md`). Once they are removed, the Mac
result can be repeated on Linux/Vulkan. The gate 5 tools in `scripts/scene/tools/` assume macOS (`--offline`
uses `sandbox-exec`), so that run needs a Linux equivalent.

## Recording results

- `docs/scene-packages.md`, "Measured": replace each *pending* with the numbers above, the date, the machine
  (CPU, cores, RAM) and the image digest.
- `ROADMAP.md`, "Realism track": mark the gates done or note any failures.
- Commit the doc changes and delete this file in the same commit.
