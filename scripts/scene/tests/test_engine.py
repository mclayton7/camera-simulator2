import io
import json
import os

import pytest

from camsim_scene import engine, tiling
from camsim_scene.engine import BuildLock, BuildLocked, LayerStats, Markers


def produce_counter(calls, data=b"tile"):
    def produce():
        calls.append(1)
        return data

    return produce


def test_run_tile_builds_then_skips(tmp_path):
    m, calls = Markers(tmp_path), []
    r1 = engine.run_tile(tmp_path, m, "terrain", "terrain", 3, 1, 2, "in-1", produce_counter(calls))
    r2 = engine.run_tile(tmp_path, m, "terrain", "terrain", 3, 1, 2, "in-1", produce_counter(calls))
    assert not r1.skipped and r2.skipped and len(calls) == 1
    assert (tmp_path / "terrain/3/1/2.terrain").read_bytes() == b"tile"
    assert m.output_for_file("terrain/3/1/2.terrain") == r1.sha256


def test_changed_inputs_or_tampered_file_rebuild(tmp_path):
    m, calls = Markers(tmp_path), []
    engine.run_tile(tmp_path, m, "imagery", "jpg", 0, 0, 0, "a", produce_counter(calls))
    engine.run_tile(tmp_path, m, "imagery", "jpg", 0, 0, 0, "b", produce_counter(calls))
    (tmp_path / "imagery/0/0/0.jpg").write_bytes(b"tampered")
    engine.run_tile(tmp_path, m, "imagery", "jpg", 0, 0, 0, "b", produce_counter(calls))
    assert len(calls) == 3 and (tmp_path / "imagery/0/0/0.jpg").read_bytes() == b"tile"


def test_failed_job_leaves_no_file_and_no_marker(tmp_path):
    def boom():
        raise KeyboardInterrupt

    with pytest.raises(KeyboardInterrupt):
        engine.run_tile(tmp_path, Markers(tmp_path), "terrain", "terrain", 1, 0, 0, "x", boom)
    assert not (tmp_path / "terrain/1/0/0.terrain").exists() and Markers(tmp_path).read("terrain", 1, 0, 0) is None


def test_build_lock_refuses_second_holder(tmp_path):
    with BuildLock(tmp_path), pytest.raises(BuildLocked, match="another build"), BuildLock(tmp_path):
        pass
    with BuildLock(tmp_path):
        pass


def test_remove_stale_keeps_exactly_the_plan(tmp_path):
    plan = tiling.LayerPlan({0: tiling.make_keys([0, 1], [0, 0]), 1: tiling.make_keys([0], [0])}, {})
    m = Markers(tmp_path)
    for rel in [
        "terrain/0/0/0.terrain",
        "terrain/0/1/0.terrain",
        "terrain/1/0/0.terrain",
        "terrain/1/3/1.terrain",
        "terrain/2/0/0.terrain",
        "terrain/0/0/.0.terrain.abc.part",
        "terrain/layer.json",
    ]:
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / rel).write_bytes(b"x")
    m.write("terrain", 1, 3, 1, "i", "o")
    m.write("terrain", 2, 0, 0, "i", "o")
    removed = engine.remove_stale(tmp_path, "terrain", "terrain", plan)
    left = sorted(str(p.relative_to(tmp_path)) for p in (tmp_path / "terrain").rglob("*") if p.is_file())
    assert left == ["terrain/0/0/0.terrain", "terrain/0/1/0.terrain", "terrain/1/0/0.terrain", "terrain/layer.json"]
    assert removed == 3 and m.read("terrain", 1, 3, 1) is None and m.read("terrain", 2, 0, 0) is None


def square(chunk):
    return [engine.TileResult("t", 0, x, 0, False, x * x, "", 0.0, os.getpid(), 0.0) for x in chunk]


def noop_init():
    pass


@pytest.mark.parametrize("jobs", [1, 2])
def test_run_pool_runs_phases_in_order(jobs):
    seen = []
    phases = [[[1, 2], [3]], [[4]]]
    engine.run_pool(square, phases, jobs, noop_init, (), lambda r: seen.append(r.x))
    assert sorted(seen[:3]) == [1, 2, 3] and seen[3] == 4


def test_layer_stats_and_progress_json():
    st = LayerStats()
    r = engine.TileResult("terrain", 1, 0, 0, False, 100, "ab", 0.5, 1, 512.0)
    st.add(r)
    st.add(engine.TileResult("terrain", 1, 1, 0, True, 50, "cd", 0.0, 2, 700.0))
    assert (st.tiles, st.built, st.skipped, st.bytes, st.peak_rss_mb) == (2, 1, 1, 150, 700.0)

    out = io.StringIO()
    p = engine.Progress("terrain", 2, json_lines=True, stream=out)
    p.update(r)
    p.done()
    lines = [json.loads(line) for line in out.getvalue().splitlines()]
    assert lines[-1]["layer"] == "terrain" and lines[-1]["done"] == 1 and lines[-1]["total"] == 2
