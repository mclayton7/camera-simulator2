"""Realism-vs-cost sweep: axes, row collapse, env mapping, aggregation, budget (REALISM R3/R4 bench)."""

import json

import pytest
from PIL import Image

from bench import sweep


def test_parse_axis_and_unknown_axis():
    assert sweep.parse_axis("buildings_sse=8,16") == ("buildings_sse", ["8", "16"])
    with pytest.raises(SystemExit, match="unknown axis 'roofs'"):
        sweep.parse_axis("roofs=flat")
    with pytest.raises(SystemExit, match="no values"):
        sweep.parse_axis("trees=")


def test_disabled_feature_collapses_its_axes():
    rows = sweep.build_rows(
        [("buildings", ["off", "on"]), ("buildings_sse", ["8", "16"]), ("trees", ["off"])]
    )
    assert rows == [
        {"buildings": "off", "trees": "off"},
        {"buildings": "on", "buildings_sse": "8", "trees": "off"},
        {"buildings": "on", "buildings_sse": "16", "trees": "off"},
    ]


def test_no_axes_is_one_baseline_row():
    rows = sweep.build_rows([])
    assert rows == [{}]
    assert sweep.row_label({}) == "baseline"


def test_env_mapping_uses_ints_for_switches():
    env = sweep.row_env({"buildings": "on", "trees_shadows": "off", "trees_radius": "1000"})
    assert env == {
        "CAMSIM_SCENE_BUILDINGS_ENABLED": "1",
        "CAMSIM_VEGETATION_CAST_SHADOWS": "0",
        "CAMSIM_VEGETATION_RADIUS_M": "1000",
    }


def test_row_label_is_filesystem_safe_and_ordered():
    assert sweep.row_label({"buildings": "on", "buildings_sse": "16"}) == "buildings-on_buildings_sse-16"
    assert sweep.row_label({"trees_density": "0.25"}) == "trees_density-0.25"


def _results(tmp_path, name, **phase):
    p = tmp_path / name / "results.json"
    p.parent.mkdir(parents=True)
    base = {"frames": 2700, "game_ms_p50": 3.0, "game_ms_p95": 4.0, "gpu_ms_p50": 20.0,
            "wall_ms_p95": 34.0, "dropped": 0, "emitted_fps": 29.9, "gpu_mem_mib_max": 3000.0}
    base.update(phase)
    p.write_text(json.dumps({"phases": {"dense_pass": base}}))
    return p


def test_run_metrics_marks_crashes_and_empty_phases_failed(tmp_path):
    assert sweep.run_metrics(tmp_path / "missing" / "results.json")["status"] == "failed"
    assert sweep.run_metrics(_results(tmp_path, "empty", frames=0))["status"] == "failed"
    ok = sweep.run_metrics(_results(tmp_path, "ok"))
    assert ok["status"] == "ok" and ok["game_ms_p95"] == 4.0


def test_aggregate_ranges_and_budget_against_the_all_off_row(tmp_path):
    rows = [{"buildings": "off", "trees": "off"}, {"buildings": "on", "trees": "on"}]
    off = [sweep.run_metrics(_results(tmp_path, f"off{i}", game_ms_p50=3.0 + i * 0.1)) for i in range(2)]
    on = [sweep.run_metrics(_results(tmp_path, f"on{i}", game_ms_p50=4.0, game_ms_p95=8.0,
                                     gpu_mem_mib_max=3300.0)) for i in range(2)]
    agg = sweep.aggregate(rows, {sweep.row_label(rows[0]): off, sweep.row_label(rows[1]): on})
    assert agg[0]["baseline"] and not agg[1]["baseline"]
    assert agg[0]["game_ms_p50"] == (3.0, 3.1)
    v = agg[1]["budget"]
    assert v["game_ms_p50"] is True  # +0.95 ms (median 4.0 vs 3.05) <= 1.5
    assert v["game_ms_p95"] is False  # +4.0 ms > 3.0
    assert v["gpu_mem_mib_max"] is True  # +300 MiB <= 381
    assert agg[1]["within_budget"] is False


def test_failed_rows_get_no_verdict_and_unknown_memory_is_not_a_pass(tmp_path):
    rows = [{"trees": "off"}, {"trees": "on"}]
    off = [sweep.run_metrics(_results(tmp_path, "off", gpu_mem_mib_max=None))]
    on = [sweep.run_metrics(tmp_path / "nope" / "results.json")]
    agg = sweep.aggregate(rows, {"trees-off": off, "trees-on": on})
    assert agg[1]["status"] == "failed" and agg[1]["budget"] == {}
    md = sweep.render_md(agg)
    assert "failed" in md and "n/a" in md


def test_contact_sheet_tiles_labelled_thumbnails(tmp_path):
    imgs = []
    for i in range(4):
        p = tmp_path / f"{i}.png"
        Image.new("RGB", (1920, 1080), (i * 60, 0, 0)).save(p)
        imgs.append((f"row{i}", p))
    imgs.append(("missing", tmp_path / "none.png"))
    out = tmp_path / "sheet.png"
    sweep.contact_sheet(imgs, out, thumb_w=320, cols=3)
    sheet = Image.open(out)
    assert sheet.width == 3 * 320
    assert sheet.height >= 2 * 180  # two rows of 16:9 thumbnails plus labels
