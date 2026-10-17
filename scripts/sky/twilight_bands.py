# Copyright CamSim Contributors. All Rights Reserved.
"""Measured twilight illuminant colour for night_check.py N9 (ROADMAP 6A, docs/night.md "Twilight colour").

Source: Spitschan, Aguirre, Brainard & Sweeney (2016), "Variation of outdoor illumination as a function of solar
elevation and light pollution", Sci. Rep. 6, 26756 (CC BY 4.0): supplementary data S1 (wavelengths) and S2 (downwelling
spectral irradiance, rural site, Cherry Springs State Park PA, June-July 2014, one spectrum a minute, with the solar
elevation). Fetched from the copy in the dreye package (github.com/neuralsignal/dreye v1.1.0), pinned by sha256.

For each spectrum: CIE 1931 2 deg XYZ (380-780 nm) -> linear sRGB (IEC 61966-2-1), R/G and B/G relative to the median
of the 20-26 deg spectra (the "daylight" reference N9 captures at +22 deg). Per solar-elevation bin (+3, 0, -3, -6 deg,
+-1 / +-0.5 deg wide) the 10th / 50th / 90th percentiles of the two ratios, of the colour temperature each implies on
the Planckian locus (in mired, relative to a 6,145 K reference: the bin's measured median CCT), and of their difference
(R/G mired - B/G mired: < 0 = off the locus toward cyan, > 0 toward magenta). Also the blackbody relative-ratio table
night_check uses to turn CamSim's ratios into the same mireds.

    uv run --with colour-science --with numpy python scripts/sky/twilight_bands.py   # writes scripts/sky/twilight_bands.json
"""

import csv
import hashlib
import json
import sys
import tempfile
import urllib.request
from pathlib import Path

import colour
import numpy as np

BASE = "https://raw.githubusercontent.com/neuralsignal/dreye/v1.1.0/dreye/datasets/spitschan2016/"
FILES = {"srep26756-s1.csv": "479dad2965c373b73052b518d33621385b80dead1d64964e19b610b363a8200e",
         "srep26756-s2.csv": "e0e3e895b3a66fc83d9f63bd02e92899a11a44b1d1707f8a6d7a05279cbe6bab"}
XYZ_TO_SRGB = np.array([[3.2404542, -1.5371385, -0.4985314], [-0.9692660, 1.8760108, 0.0415560],
                        [0.0556434, -0.2040259, 1.0572252]])
REF_BIN = (20.0, 26.0)
BINS = {"p3": (2.0, 4.0), "z0": (-0.5, 0.5), "m3": (-3.5, -2.5), "m6": (-6.5, -5.5)}
OUT = Path(__file__).resolve().parent / "twilight_bands.json"


def fetch(dest: Path) -> None:
    for name, sha in FILES.items():
        p = dest / name
        if not p.exists():
            urllib.request.urlretrieve(BASE + name, p)
        got = hashlib.sha256(p.read_bytes()).hexdigest()
        if got != sha:
            sys.exit(f"{name}: sha256 {got} != {sha}")


def main() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        fetch(d)
        wl = np.array([float(x) for x in (d / "srep26756-s1.csv").read_text().split()])
        rows = list(csv.reader((d / "srep26756-s2.csv").open()))
    # Header row: timestamps (+ a trailing empty column, dropped as the dataset's own loader does); then date number,
    # solar elevation, lunar elevation, moon fraction; then one row per wavelength.
    el = np.array([float(x) for x in rows[2][:-1]])
    spec = np.array([[float(x) if x not in ("NaN", "") else 0.0 for x in row[:-1]] for row in rows[5:]])
    spec = np.clip(np.nan_to_num(spec), 0.0, None)
    lam = np.arange(380.0, 781.0, 1.0)
    cmfs = colour.MSDS_CMFS["CIE 1931 2 Degree Standard Observer"]
    xyz_bar = np.stack([np.interp(lam, cmfs.wavelengths, cmfs.values[:, i]) for i in range(3)], 1)
    s = np.stack([np.interp(lam, wl, spec[:, i]) for i in range(spec.shape[1])], 1)
    xyz = xyz_bar.T @ s
    rgb = XYZ_TO_SRGB @ xyz
    ok = xyz[1] > 0
    ref = (el >= REF_BIN[0]) & (el <= REF_BIN[1]) & ok
    ref_rg, ref_bg = float(np.median(rgb[0, ref] / rgb[1, ref])), float(np.median(rgb[2, ref] / rgb[1, ref]))
    xy_ref = (xyz[:2, ref] / xyz[:, ref].sum(0)).mean(axis=1)
    ref_cct = float(colour.temperature.xy_to_CCT(xy_ref, method="Hernandez 1999"))

    temps = np.exp(np.linspace(np.log(2000.0), np.log(1.0e6), 600))

    def bb_ratios(t: float) -> tuple[float, float]:
        m = lam * 1e-9
        c = XYZ_TO_SRGB @ (xyz_bar.T @ (1.0 / m**5 / (np.exp(1.4388e-2 / (m * t)) - 1.0)))
        return float(c[0] / c[1]), float(c[2] / c[1])

    r0 = bb_ratios(ref_cct)
    table = [(float(t), *(x / y for x, y in zip(bb_ratios(t), r0))) for t in temps]
    rel_rg_t = np.array([r[1] for r in table])
    rel_bg_t = np.array([r[2] for r in table])

    def mired_rg(x: float) -> float:   # rel R/G falls with T; beyond the locus end (T = inf): 0 mired
        return 0.0 if x <= rel_rg_t[-1] else 1e6 / float(np.interp(-x, -rel_rg_t, temps))

    def mired_bg(x: float) -> float:
        return 0.0 if x >= rel_bg_t[-1] else 1e6 / float(np.interp(x, rel_bg_t, temps))

    out = {"source": "Spitschan et al. 2016, Sci. Rep. 6, 26756, supplementary S2 (rural site)",
           "files_sha256": FILES, "reference_bin_deg": list(REF_BIN), "reference_cct_k": ref_cct,
           "reference_rg": ref_rg, "reference_bg": ref_bg, "bins": {},
           "blackbody_rel": {"temps_k": [r[0] for r in table], "rg": rel_rg_t.tolist(), "bg": rel_bg_t.tolist()}}
    for name, (lo, hi) in BINS.items():
        m = (el >= lo) & (el <= hi) & ok
        rg = rgb[0, m] / rgb[1, m] / ref_rg
        bg = rgb[2, m] / rgb[1, m] / ref_bg
        mr = np.array([mired_rg(x) for x in rg])
        mb = np.array([mired_bg(x) for x in bg])
        pct = lambda a: [float(np.percentile(a, p)) for p in (10, 50, 90)]  # noqa: E731
        out["bins"][name] = {"elevation_deg": [lo, hi], "n": int(m.sum()), "rg": pct(rg), "bg": pct(bg),
                             "mired_rg": pct(mr), "mired_bg": pct(mb), "dmired": pct(mr - mb)}
        print(name, json.dumps(out["bins"][name]))
    OUT.write_text(json.dumps(out, indent=1) + "\n")
    print(f"reference {ref_cct:.0f} K; wrote {OUT}")


if __name__ == "__main__":
    main()
