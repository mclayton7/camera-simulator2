# Gate tools (REALISM R0 gate 5)

| File | What it does |
|---|---|
| `render_check.py` | Launches CamSim on a package (`package PKG`) or on ion (`cwt`), flies the fixed shots (incl. `coast_low`, and `coast_waves` in a second session at Beaufort 5), saves `/snapshot` PNGs, times each shot's terrain readiness (`ready_s`), and fails on any terrain-gate timeout in the log (`gate_timeouts`) |
| `hot_check.py` | CIGI frame-centre heights and HAT/HOT (opcode 24 → 103) vs 3DEP truth; PASS/FAIL at 0.25 m (1 m data) / 1.0 m (1/3″); plus HOT at three sea points vs EGM96 + `sea_level.json` offset (0.05 m, Beaufort 0) |
| `water_check.py` | R1 water gate: open-water leaves vs the raw Sentinel-2 decode, uniformity, and (`--before-hashes`) no leaf without water changed |
| `coast_check.py` | R1 coast gate: package terrain along coast transects vs the drawn sea (EGM96 + `sea_level.json`): offshore below sea, no step at the old 3DEP edge, waterline vs WorldCover, offset vs pyproj; `--before PKG` compares |
| `ndvi_check.py` | R1 chunk 2 NDVI gates: median NDVI per WorldCover class (plausibility), the bias across NAIP's edge (seam; also with the global fit only and unfitted), `--overview` false-colour PNG |
| `registration.py` | Phase-correlation shift between a package shot and the CWT shot of the same pose (< 1 px passes) |
| `camsim_session.py` | Shared by the two launchers: package/ion environment, `run.sh --detach` + CIGI host + `/ready`, `stop.sh` |
| `offline.sb` | macOS `sandbox-exec` profile blocking ports 80/443 (`render_check.py --offline`, which also sets `CAMSIM_SCENE_OFFLINE=1` and fails on any logged request) |
