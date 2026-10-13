#!/usr/bin/env python3
# Copyright CamSim Contributors. All Rights Reserved.
"""Replace library.json entries (by name) with the JSON objects prepare_tree.py printed (REALISM R4; build_library.sh).

  python3 scripts/vegetation/install_entries.py LIBRARY_JSON ENTRIES_TXT
"""
import json
import sys
from pathlib import Path


def merge(library: dict, entries: list[dict]) -> dict:
    """Entries replace the library's types of the same name in place; new names are appended."""
    by_name = {t["name"]: k for k, t in enumerate(library["types"])}
    for e in entries:
        if e["name"] in by_name:
            library["types"][by_name[e["name"]]] = e
        else:
            library["types"].append(e)
    return library


def read_entries(text: str) -> list[dict]:
    dec, out, i = json.JSONDecoder(), [], 0
    while (i := text.find("{", i)) >= 0:
        obj, i = dec.raw_decode(text, i)
        out.append(obj)
    return out


if __name__ == "__main__":
    lib = Path(sys.argv[1])
    merged = merge(json.loads(lib.read_text()), read_entries(Path(sys.argv[2]).read_text()))
    lib.write_text(json.dumps(merged, indent=2) + "\n")
    print([t["name"] for t in merged["types"]])
