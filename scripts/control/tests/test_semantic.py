"""The Python class table must equal CamSim's (GroundTruth/SemanticClasses.cpp), so datasets can't drift from it."""

from __future__ import annotations

import re
from pathlib import Path

from camsim_control.semantic import CLASSES

CPP = Path(__file__).resolve().parents[3] / "unreal_project/CamSimTest/Source/CamSimTest/GroundTruth/SemanticClasses.cpp"
HDR = CPP.with_suffix(".h")


def test_matches_cpp():
    ids = {name: int(v) for name, v in re.findall(r"\b([A-Z][A-Za-z]+) = (\d+)", HDR.read_text())}
    names = dict(re.findall(r"\{ ([A-Z][A-Za-z]+), TEXT\(\"([a-z_]+)\"\) \}", CPP.read_text()))
    cpp = {ids[k]: v for k, v in names.items()}
    assert len(cpp) == 17
    assert CLASSES == cpp
