"""NAIP -> Sentinel-2 NDVI fit (REALISM R1 chunk 2; docs/superpowers/specs/2026-10-09-ndvi-sun-metadata-design.md).

NAIP NDVI comes from uncalibrated DN; one linear map (gain, offset), least squares on the land lattice the colour
match uses (balance.py), puts it on Sentinel-2's near-reflectance scale. Stored as ndvi/fit.json in integer
millionths, so float summation order can't change the file; workers read it back from the file."""

from __future__ import annotations

import json
import logging
from dataclasses import dataclass, field

import numpy as np

from .balance import shared_land_samples
from .manifest import canonical_json
from .tiling import Bounds

log = logging.getLogger(__name__)
FILE = "fit.json"
FORMAT = 1
MICRO = 1_000_000


@dataclass
class NdviFit:
    reference: str
    target: str
    gain_e6: int
    offset_e6: int
    report: dict = field(default_factory=dict, compare=False)

    @classmethod
    def make(cls, reference: str, target: str, gain: float, offset: float) -> NdviFit:
        return cls(reference, target, int(round(gain * MICRO)), int(round(offset * MICRO)))

    @property
    def gain(self) -> float:
        return self.gain_e6 / MICRO

    @property
    def offset(self) -> float:
        return self.offset_e6 / MICRO

    def apply(self, n) -> np.ndarray:
        """Reference NDVI on the target's scale, clamped to [-1, 1]; NaN stays NaN."""
        return np.clip(self.gain * np.asarray(n, np.float64) + self.offset, -1.0, 1.0)

    def to_json(self) -> bytes:
        d = {
            "format": FORMAT,
            "reference": self.reference,
            "target": self.target,
            "gain_e6": self.gain_e6,
            "offset_e6": self.offset_e6,
        }
        return canonical_json(d).encode()

    @classmethod
    def from_json(cls, data: bytes) -> NdviFit:
        d = json.loads(data)
        if d.get("format") != FORMAT:
            raise ValueError(f"ndvi fit format {d.get('format')!r} != {FORMAT}")
        return cls(d["reference"], d["target"], int(d["gain_e6"]), int(d["offset_e6"]))


def linear_fit(x: np.ndarray, y: np.ndarray) -> tuple[float, float]:
    """Least-squares (gain, offset) for y ~ gain * x + offset."""
    gain, offset = np.polyfit(np.asarray(x, np.float64), np.asarray(y, np.float64), 1)
    return float(gain), float(offset)


def _r4(v) -> float | None:
    return None if v is None or not np.isfinite(v) else round(float(v), 4)


def fit_ndvi(index, classes, bounds: Bounds, s: dict, dates: dict | None = None) -> NdviFit | None:
    """Fit on lattice nodes over `bounds` (the reference's footprints) where the reference and the target are both
    valid and the land cover isn't excluded: even (i + j) nodes fit, odd ones are held out for the report, which gives
    each reference acquisition date's residual bias (`dates`: reference qid -> date). None with fewer than
    s["min_samples"] fit samples."""
    from .layers.ndvi import sample_ndvi

    dates = dates or {}
    parts = []
    for b in shared_land_samples(index, classes, bounds, s, sample_ndvi):
        k = b.keep
        day = np.array([dates.get(e.qid, "") for e in b.ref_entries], dtype=object)[b.ref_which[k]]
        parts.append((b.ref_values[k], b.target_values[k], b.held[k], day))
    if not parts:
        log.warning("ndvi fit: no shared land samples between %s and %s", s["reference"], s["target"])
        return None
    x, y, held, day = (np.concatenate([p[k] for p in parts]) for k in range(4))
    fit = ~held
    if fit.sum() < s["min_samples"]:
        log.warning("ndvi fit: only %d shared land samples; not fitted", int(fit.sum()))
        return None
    f = NdviFit.make(s["reference"], s["target"], *linear_fit(x[fit], y[fit]))
    before, after = y[held] - x[held], y[held] - f.apply(x[held])
    by_date = {}
    for d in sorted(set(day[held].tolist())):
        m = day[held] == d
        by_date[d or "unknown"] = {"n": int(m.sum()), "bias": _r4(np.median(after[m]))}
    has = before.size > 0
    f.report = {
        "samples_fit": int(fit.sum()),
        "samples_heldout": int(held.sum()),
        "gain": f.gain,
        "offset": f.offset,
        "heldout": {
            "mae_before": _r4(np.abs(before).mean()) if has else None,
            "mae_after": _r4(np.abs(after).mean()) if has else None,
            "bias_before": _r4(np.median(before)) if has else None,
            "bias_after": _r4(np.median(after)) if has else None,
        },
        "bias_after_by_date": by_date,
    }
    log.info("ndvi fit: gain %.6f offset %.6f, held out %s", f.gain, f.offset, json.dumps(f.report["heldout"]))
    return f
