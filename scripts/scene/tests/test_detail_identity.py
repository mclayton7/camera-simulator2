"""REALISM R5 gate 1: without [detail], planning is byte-identical to before R5 (goldens made before the change)."""

from __future__ import annotations

from pathlib import Path

import pytest

from camsim_scene.config import layer_settings, load_scene
from camsim_scene.manifest import canonical_json

EXAMPLES = Path(__file__).resolve().parents[1] / "examples"
GOLDEN = Path(__file__).parent / "fixtures" / "plan_golden"


@pytest.mark.parametrize("name", ["pendleton", "pendleton-preview-10k", "pendleton-r5veg"])
def test_plan_without_detail_is_unchanged(name):
    plan = load_scene(EXAMPLES / f"{name}.toml")
    got = canonical_json({"regions": [r.to_dict() for r in plan.regions()], "layers": layer_settings(plan)})
    assert got == (GOLDEN / f"{name}.json").read_text(encoding="utf-8")
