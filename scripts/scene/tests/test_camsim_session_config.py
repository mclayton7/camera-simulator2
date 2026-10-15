"""camsim_session.config_override: a temporary run.sh config that leaves the project config as deploy's afterwards."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import camsim_session as cs  # noqa: E402


def test_override_is_used_and_the_project_config_is_restored(tmp_path):
    deploy, project = tmp_path / "deploy.yaml", tmp_path / "project.yaml"
    deploy.write_text("k1: 0.0\n")
    with cs.config_override("k1: -0.2\n", deploy=deploy, project=project) as env:
        src = Path(env["CAMSIM_CONFIG_SRC"])
        assert src.read_text() == "k1: -0.2\n"
        project.write_text(src.read_text())  # what run.sh does at launch
    assert not src.exists() and project.read_text() == "k1: 0.0\n"


def test_the_project_config_is_restored_after_an_error(tmp_path):
    deploy, project = tmp_path / "deploy.yaml", tmp_path / "project.yaml"
    deploy.write_text("a: 1\n")
    with pytest.raises(RuntimeError), cs.config_override("a: 2\n", deploy=deploy, project=project):
        project.write_text("a: 2\n")
        raise RuntimeError("boom")
    assert project.read_text() == "a: 1\n"
