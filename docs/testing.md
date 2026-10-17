# Quick checks while developing

Run the narrowest check that covers the change while iterating, and the full suites before a merge or a milestone.
Every acceptance script can run a subset; this page says which subset fits which change. Commands, the full test
list and the toolchain notes are in `CLAUDE.md` ("Testing").

Times measured 2026-10-13 on the Linux reference box (Core Ultra 9 285K, RTX 5080), warm shader and tile caches.

## Unit tests (seconds to minutes)

| Suite | Full run | Narrow run |
|---|---|---|
| C++, NullRHI (709 tests) | ~80 s: 6 s editor start, 27 s inside the tests, ~45 s of the automation controller's per-test overhead | `RunTests CamSim.<Group>` in the command from `CLAUDE.md`, e.g. `CamSim.Vegetation` (84 tests: 20 s end-to-end), `CamSim.Thermal` (141), `CamSim.Sensor.Physics`, or one test's full path: the time scales with the test count |
| C++, GPU (`CamSim.GPU.*`, 27) | a few minutes | `scripts/run_gpu_tests.sh CamSim.GPU.Thermal` (groups: `Sensor`, `Thermal`, `GroundTruth`, `Entity`, `LandCover`, `Ocean`) |
| Python, `scripts/tests` (300) | ~1 s | `-k <name>` |
| Python, control API client (`scripts/control`, 17) | ~3 s | `uv run --project scripts/control --with pytest pytest scripts/control/tests -k <name>` |
| Python, scene tooling (748) | 37 s with `-n auto` (24 cores; 141 s serial) | one module: `uv run --project scripts/scene pytest scripts/scene/tests/test_buildings_layer.py`; the parallel floor is the slowest test (~20 s, `test_ndvi_check.py`) |

A NullRHI run exits 1 even when every test passes (CesiumRuntime's `LogClass: Error` at startup): judge it by
`failed` in the report's `index.json`, as CI does.

The scene tests keep ~4 GB of temp files (pytest keeps its last three runs): point `TMPDIR` at a large disk if `/tmp`
is small.

## Acceptance scripts (minutes to an hour)

Each launches CamSim one or more times (~15–20 s to ready, plus terrain loading and warm-up per launch), so they
dominate a development loop. Narrow them:

| You changed | Full check | Narrow check while iterating |
|---|---|---|
| Thermal model, `ThermalCS`, land-cover classes | `thermal_check.py --band both` (~60 min) | `--band mwir --runs bands` (one launch: gates a–e and h–j; add `lcoff` for k); add `entity` for 4C, `pan` for the re-centre hitch, `eo` for the EO-untouched gate |
| Roof or road thermal (R2, R3b) | `--runs roofs,roads --scene PKG` | `--runs roofs --scene PKG` |
| Ocean waves, sea level, boat clamp | `ocean_check.py OUT --cigi` (~35 min, four launches) | `--beaufort 3` (one launch); `--views NAME` for one view; `--check-only` re-checks a finished run without launching |
| Ground truth (masks, boxes, depth) | `gt_occlusion_check.py OUT` | `--runs NAME`; `--check-only` |
| Vegetation streaming, scatter, far tier | `vegetation_check.py … --stages approach,off,gt,truck,oblique,footprints,r4_identity,long_range` (~40 min) | `--stages approach` (placement, species, canopy, determinism, grounding, handoff); `--stages long_range` for the far tier; `--stages footprints` for the building mask. The gates of stages not run are kept from the last `report.json` |
| Anything on the video / KLV path | `scripts/ci_validate.sh --native` (~2 min) | the same; it is already the short end-to-end check |
| KLV geometry (sensor pose, FOV, frame centre, corners), camera rig, entity placement | `georeg_check.py --scene PKG --out OUT` (R6, `docs/realism-validation.md`) | `--runs main --views flat/nadir,flat/corner` (one launch, two views); `--check-only` re-evaluates a finished run without launching |
| Bridge decks (`camsim_scene/bridges.py`, the bridges layer, the DIS deck ceiling) | `scripts/scene/tools/bridges_check.py --scene PKG --out OUT` (three launches) | `--runs hot` (one launch: B2, B3); the scene tests `test_bridges*.py`; `RunTests CamSim.Entity.SurfaceClamp+CamSim.Scene.Bridges` |
| Night sky, ephemeris, illumination, sky dome, cached-lighting band, radiometric EO AE | `night_check.py --scene PKG --out OUT --baseline BASE` (6A, `docs/night.md`; ~25 min without `thermal`) | `--runs moon` (N2, N3: one launch), `--runs dusk` (N5, N8), `--runs stars` (N4); `--check-only` re-evaluates; `RunTests CamSim.Environment+CamSim.Sensor` |
| Datum pipelines (`camsim_scene/datum.py`, GEOID18) | `scripts/scene/tools/datum_check.py PKG` (seconds, no CamSim) | the same |
| Anything that could make frames vary run to run (sensor noise, streaming, vegetation, TSR) | `determinism_check.py --scene PKG --out OUT` (two launches, six poses) | `--check-only` re-compares a finished pair |
| Control API (`Control/`, capture tagging, lockstep sim delta, `scripts/control/`) | `control_api_check.py --scene PKG --out OUT` (two launches, gates C1–C8, ~15 min) | `RunTests CamSim.ControlApi` (NullRHI, routes in-process); `--runs main` (one launch: C1–C6, C8) |
| Semantic classes (`WRITE_SEMANTIC`, `SemanticReference`, class ids, the semantic readback) | `semantic_check.py --scene PKG --out OUT` (S1–S6; three launches) | `RunTests CamSim.GroundTruth.Semantic` (NullRHI) + `run_gpu_tests.sh CamSim.GPU.GroundTruth.Semantic`; `--runs main` (one launch, S1–S5) |
| Render cost | `run_bench.py --site pendleton` (all phases) | `--phases dense_pass --shots dense_oblique`, or `--smoke` (one shot, no measured phase). A sweep row is ~5.5 min, of which the 210 s warm-up is fixed |

Before merging, run the full NullRHI and GPU suites and the full acceptance script of every subsystem touched.
