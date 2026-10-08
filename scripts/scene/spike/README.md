# Realism R0 spike (throwaway reference code)

Spike code from 2026-10-07 (`docs/realism-r0-spike.md`). It is **not** the R0 tooling: it exists so
the R0 implementation can reuse what was learned (see the report's "Pitfalls" section). Expect it to
be deleted once `scripts/scene/` has the real build engine.

| File | What it does |
|---|---|
| `gen_terrain.py` | 3DEP (1/3" + 1 m) → NAVD88/NAD83(2011) to ITRF2014 ellipsoid → quantized-mesh pyramid + `layer.json`, then verifies decoded vertex heights against the source |
| `gen_imagery.py` | NAIP (Planetary Computer, 2022) → geodetic TMS (`tilemapresource.xml`) |
| `pyramid_down.py` | Builds the lower imagery zooms (down to 0) from an existing level |
| `qm_decode.py` | Minimal quantized-mesh vertex decoder (verification) |
| `spike_run.py` | Launches CamSim with a local package (`local`), local terrain only (`localterrain`) or ion (`cwt`) and grabs `/snapshot` shots |
| `hot_check.py` | Frame-centre (CIGI 107) altitude vs 3DEP truth at nadir points |
| `datum_check.py`, `naip_query.py`, `dem_coverage.py` | Datum, NAIP and 1 m DEM coverage probes |
| `offline.sb` | macOS `sandbox-exec` profile that blocks ports 80/443 (offline run) |
| `tms_overlay_spike.patch` | The throwaway `imagery.source: tms` branch used for the runs (not merged) |

```bash
cd scripts/scene/spike && uv sync   # Python 3.12-3.13 (pydelatin wheels)
uv run python gen_terrain.py --bbox -117.41 33.20 -117.35 33.25 --third <1/3" COG url> --onem <1 m COG url> --maxz 16 --out pkg/terrain
uv run python gen_imagery.py --bbox -117.41 33.20 -117.35 33.25 --out pkg/imagery && uv run python pyramid_down.py pkg/imagery 10
git apply tms_overlay_spike.patch && ../../run.sh --build-only   # local TMS imagery needs the patch
uv run python spike_run.py out/local local "$PWD/pkg"
```
