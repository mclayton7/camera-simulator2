"""Source adapter registry. A built-in id maps to its class; "module:Class" names any importable adapter
(the test suite's synthetic sources). Imports are lazy so this package stays light."""

from __future__ import annotations

import importlib

BUILTIN = {
    "dep3_13": "camsim_scene.sources.dep3_13:Dep313",
    "dep3_1m": "camsim_scene.sources.dep3_1m:Dep31m",
    "naip_pc": "camsim_scene.sources.naip_pc:NaipPc",
    "wc_s2": "camsim_scene.sources.wc_s2:WcS2",
    "etopo2022": "camsim_scene.sources.etopo2022:Etopo2022",
    "bmng": "camsim_scene.sources.bmng:Bmng",
    "worldcover": "camsim_scene.sources.worldcover:WorldCover",
    "noaa_sd13": "camsim_scene.sources.noaa_dem:NoaaSd13",
    "noaa_crm_socal": "camsim_scene.sources.noaa_dem:NoaaCrmSocal",
}


class UnknownSource(KeyError):
    pass


def make_source(source_id: str, adapter: str | None = None, options: dict | None = None, http=None):
    name = adapter or source_id
    spec = BUILTIN.get(name, name)
    if ":" not in spec:
        raise UnknownSource(f"unknown source adapter {name!r} (built-ins: {sorted(BUILTIN)})")
    module, cls = spec.split(":", 1)
    src = getattr(importlib.import_module(module), cls)(options or {}, http)
    src.id = source_id
    return src
