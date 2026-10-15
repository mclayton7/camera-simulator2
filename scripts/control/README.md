# camsim-control

Python client for CamSim's control API (ROADMAP 2.4). Guide: [`docs/control-api.md`](../../docs/control-api.md).

```python
from camsim_control import Client

c = Client("http://127.0.0.1:8080")
c.wait_ready()
c.set_clock(lockstep=True, utc="2026-06-15T19:00:00Z")
c.put_entity(1, type_id=2001, lat=33.3073, lon=-117.3446, alt=20, heading=45, surface="ground")
c.set_camera(lat=33.3010, lon=-117.3446, alt=720, heading=0, gimbal={"pitch": -45}, hfov_deg=15,
             sensor={"mode": "ir"})
cap = c.capture()            # waits for the scene to settle; image, depth and annotations of that frame
open("frame.png", "wb").write(cap.png)
```

- `make_dataset.py`: a seeded COCO dataset (EO and/or IR per sample).
- Tests: `uv run --project scripts/control --with pytest pytest scripts/control/tests` (no CamSim needed).
