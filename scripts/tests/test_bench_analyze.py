"""Per-phase metrics come out of frame-stats rows as the harness expects."""
from bench import analyze


def _row(t, wall, gpu=10.0, load=100.0, fam=1, dropped=0, emitted=0):
    return {"t": t, "wall_ms": wall, "game_ms": 3.0, "render_ms": 5.0, "rhi_ms": 1.0,
            "gpu_ms": gpu, "emitted": emitted, "dropped": dropped, "load_pct": load,
            "sse": 16.0, "cut": False, "families": fam}


def test_rows_are_split_by_phase_window_and_unmeasured_phases_skipped():
    rows = [_row(t, 33.0) for t in range(0, 10)] + [_row(t, 40.0) for t in range(10, 20)]
    phases = [{"name": "warmup", "start": 0, "end": 10, "measured": False},
              {"name": "orbit", "start": 10, "end": 20, "measured": True}]
    out = analyze.summarize(rows, phases)
    assert list(out) == ["orbit"]
    assert out["orbit"]["frames"] == 10
    assert out["orbit"]["wall_ms_p50"] == 40.0


def test_hitches_popin_families_and_drops():
    rows = [_row(0, 33.0), _row(1, 70.0, load=90.0), _row(2, 120.0, fam=2), _row(3, 33.0, load=99.9, dropped=3)]
    out = analyze.summarize(rows, [{"name": "slew", "start": 0, "end": 4, "measured": True}])["slew"]
    assert out["hitches_66"] == 2       # 70 and 120
    assert out["hitches_100"] == 1      # 120
    assert out["popin_fraction"] == 0.5
    assert out["families_mean"] == 1.25
    assert out["dropped"] == 3          # counter delta over the phase


def test_emitted_fps_is_frames_handed_to_the_encoder_per_second():
    # 31 ticks over 1 s (t = 0 .. 1), 15 frames emitted: the half-rate stream.
    rows = [_row(i / 30.0, 33.3, emitted=i // 2) for i in range(31)]
    out = analyze.summarize(rows, [{"name": "orbit", "start": 0, "end": 2, "measured": True}])["orbit"]
    assert abs(out["emitted_fps"] - 15.0) < 0.01


def test_empty_phase_reports_zero_frames_not_an_error():
    out = analyze.summarize([], [{"name": "orbit", "start": 0, "end": 1, "measured": True}])
    assert out["orbit"]["frames"] == 0
    assert out["orbit"]["emitted_fps"] == 0.0


def test_load_rows_skips_a_partial_last_line_from_a_crashed_run(tmp_path, capsys):
    p = tmp_path / "frames.jsonl"
    p.write_text('{"t": 0, "wall_ms": 33.0}\n{"t": 1, "wall_ms": 34.0}\n{"t": 2, "wal')
    rows = analyze.load_rows(p)
    assert [r["t"] for r in rows] == [0, 1]
    assert "skipped 1" in capsys.readouterr().err
