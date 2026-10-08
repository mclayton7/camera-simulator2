# Gate tools (REALISM R0 gate 5)

| File | What it does |
|---|---|
| `render_check.py` | Launches CamSim on a package (`package PKG`) or on ion (`cwt`), flies seven shots, saves `/snapshot` PNGs |
| `hot_check.py` | CIGI frame-centre heights vs 3DEP truth; PASS/FAIL at 0.25 m (1 m data) / 1.0 m (1/3″) |
| `registration.py` | Phase-correlation shift between a package shot and the CWT shot of the same pose (< 1 px passes) |
| `camsim_session.py` | Shared by the two launchers: package/ion environment, `run.sh --detach` + CIGI host + `/ready`, `stop.sh` |
| `tms_overlay.patch` | The throwaway `imagery.source: tms` branch (R1 lands the real one). Apply locally, never commit |
| `offline.sb` | macOS `sandbox-exec` profile blocking ports 80/443 (`render_check.py --offline`) |
