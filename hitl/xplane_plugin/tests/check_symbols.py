#!/usr/bin/env python3
# Copyright CamSim Contributors. All Rights Reserved.
"""Check the Linux .xpl's dynamic symbol table (nm -D):

- defined (exported): exactly the five X-Plane plugin entry points;
- undefined XPLM symbols: only the read-only set the plugin is meant to use
  (no XPLMSetData*, nothing that changes X-Plane settings);
- no libstdc++/libgcc_s dependency.

usage: check_symbols.py <CamSimTruth.xpl>
"""
import subprocess
import sys

EXPORTS = {"XPluginStart", "XPluginStop", "XPluginEnable", "XPluginDisable", "XPluginReceiveMessage"}
ALLOWED_XPLM = {
    "XPLMDebugString", "XPLMEnableFeature", "XPLMGetMyID", "XPLMGetPluginInfo",
    "XPLMFindDataRef", "XPLMGetDataRefTypes", "XPLMGetDatai", "XPLMGetDataf", "XPLMGetDatad",
    "XPLMGetDatavf", "XPLMGetDatavi",
    "XPLMGetCycleNumber", "XPLMCreateFlightLoop", "XPLMScheduleFlightLoop", "XPLMDestroyFlightLoop",
    "XPLMCreateProbe", "XPLMDestroyProbe", "XPLMProbeTerrainXYZ",
    "XPLMWorldToLocal", "XPLMLocalToWorld",
}


def nm(path, *flags):
    out = subprocess.run(["nm", "-D", *flags, path], check=True, capture_output=True, text=True).stdout
    return {line.split()[-1].split("@")[0] for line in out.splitlines() if line.strip()}


def main() -> int:
    path = sys.argv[1]
    defined = nm(path, "--defined-only")
    undefined = nm(path, "--undefined-only")
    errors = []

    exported = {s for s in defined if not s.startswith("_")}  # _init/_fini/_edata etc. are linker-made
    if exported != EXPORTS:
        errors.append(f"exports {sorted(exported)} != {sorted(EXPORTS)}")

    xplm = {s for s in undefined if s.startswith("XPLM")}
    if xplm - ALLOWED_XPLM:
        errors.append(f"unexpected XPLM imports: {sorted(xplm - ALLOWED_XPLM)}")
    if any(s.startswith("XPLMSet") for s in xplm):
        errors.append("plugin writes datarefs")
    other = sorted(s for s in undefined if not s.startswith("XPLM"))

    needed = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
    for lib in ("libstdc++", "libgcc_s", "XPLM"):
        if lib in needed:
            errors.append(f"links {lib}")

    print("exports:", " ".join(sorted(exported)))
    print("XPLM imports:", " ".join(sorted(xplm)))
    print("libc imports:", " ".join(other))
    for e in errors:
        print("FAIL", e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
