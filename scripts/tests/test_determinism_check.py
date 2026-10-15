"""Unit tests for scripts/determinism_check.py (REALISM R6 determinism of settled stills)."""

import dataclasses

import determinism_check as dc
import numpy as np
import pytest


class Clock:
    def __init__(self):
        self.t = 0.0

    def now(self):
        return self.t

    def sleep(self, s):
        self.t += s


def frames(*values):
    """Fake /snapshot results: 4 x 4 RGB frames filled with each value in turn (the last repeats)."""
    seq = [np.full((4, 4, 3), v, np.uint8) for v in values]

    def fetch():
        return seq.pop(0) if len(seq) > 1 else seq[0]

    return fetch


def test_settle_waits_for_three_identical_frames_in_a_row():
    c = Clock()
    r = dc.settle(frames(1, 2, 3, 3, 4, 5, 5, 5), max_s=90, need=3, now=c.now, sleep=c.sleep)
    assert r["settled"] and r["frames"] == 8 and (r["image"] == 5).all()
    assert r["hash"] == dc.pixel_hash(np.full((4, 4, 3), 5, np.uint8))


def test_settle_gives_up_after_max_s():
    c = Clock()
    n = iter(range(10**6))
    r = dc.settle(lambda: np.full((4, 4, 3), next(n) % 256, np.uint8), max_s=90, need=3, now=c.now, sleep=c.sleep)
    assert not r["settled"] and 89 <= r["seconds"] <= 92


def test_settle_skips_missing_snapshots():
    c = Clock()
    seq = [None, np.zeros((2, 2, 3), np.uint8), None, np.zeros((2, 2, 3), np.uint8), np.zeros((2, 2, 3), np.uint8)]
    r = dc.settle(lambda: seq.pop(0) if seq else np.zeros((2, 2, 3), np.uint8), max_s=90, need=3, now=c.now,
                  sleep=c.sleep)
    assert r["settled"]


def test_compare_reports_equal_and_different_frames():
    a = np.zeros((10, 20, 3), np.uint8)
    b = a.copy()
    b[2:4, 5:9] = 7
    assert dc.diff_stats(a, a) == {"equal": True, "pixels": 0, "max": 0, "bbox": None}
    assert dc.diff_stats(a, b) == {"equal": False, "pixels": 8, "max": 7, "bbox": [5, 2, 9, 4]}


def test_datagram_digest_ignores_the_frame_counter_and_tracks_the_pose():
    p = dc.POSES[0].pose
    assert dc.datagram_digest(p) == dc.datagram_digest(p)
    assert dc.datagram_digest(p) != dc.datagram_digest(dataclasses.replace(p, alt=p.alt + 1))


def test_six_poses_two_in_ir():
    assert [p.name for p in dc.POSES] == ["camp_nadir", "trees_oblique", "slant_5km", "coast_waves", "camp_nadir_ir",
                                          "trees_oblique_ir"]
    assert [p.pose.sensor_id for p in dc.POSES] == [0, 0, 0, 0, 1, 1]


def test_ssim_is_one_for_identical_and_lower_for_noise():
    rng = np.random.default_rng(0)
    a = rng.integers(0, 255, (64, 64, 3), dtype=np.uint8)
    assert dc.ssim(a, a) == 1.0
    b = np.clip(a.astype(int) + rng.integers(-120, 120, a.shape), 0, 255).astype(np.uint8)
    assert dc.ssim(a, b) < 0.95


def test_snapshot_fetcher_requests_the_health_base_plus_snapshot():
    import io

    from PIL import Image

    png = io.BytesIO()
    Image.fromarray(np.zeros((2, 3, 3), np.uint8)).save(png, format="PNG")
    seen = []

    class Resp(io.BytesIO):
        status = 200

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

    def opener(url, timeout):
        seen.append(url)
        return Resp(png.getvalue())

    img = dc.snapshot_fetcher("http://127.0.0.1:8080", opener)()
    assert seen == ["http://127.0.0.1:8080/snapshot"] and img.shape == (2, 3, 3)


def test_snapshot_fetcher_returns_none_on_a_failed_request():
    def opener(url, timeout):
        raise OSError("connection refused")

    assert dc.snapshot_fetcher("http://127.0.0.1:8080", opener)() is None


def test_settle_reports_how_much_the_last_two_frames_differ():
    c = Clock()
    n = iter(range(10**6))

    def fetch():
        img = np.zeros((4, 4, 3), np.uint8)
        img[0, 0] = next(n) % 3  # one pixel flickers by up to 2 DN
        return img

    r = dc.settle(fetch, max_s=10, need=3, now=c.now, sleep=c.sleep)
    assert not r["settled"]
    assert r["last_change"] == {"equal": False, "pixels": 1, "max": 1, "bbox": [0, 0, 1, 1]}


def test_manual_exposure_config_turns_off_ae_and_agc():
    src = (dc.REPO / "deploy" / "camsim_config.yaml").read_text()
    out = dc.manual_exposure_config(src)
    assert "      manual_gain_ev: -14\n      auto: false\n  ir:" in out
    assert "    agc_enabled: false\n" in out and "    agc_enabled: true\n" not in out
    assert "      manual_gain_ev: -1\n      auto: false\n# NVG" in out
    yaml = pytest.importorskip("yaml")
    cfg = yaml.safe_load(out)
    modes = cfg["sensor_modes"]
    assert modes["eo"]["exposure"]["auto"] is False and modes["ir"]["thermal_exposure"]["auto"] is False


def test_exec_cmds_pins_and_optional_clouds_off():
    import determinism_check as dc

    base = dc.exec_cmds(clouds_off=False)
    assert base == "-ExecCmds=camsim.Sensor.FixedNoiseFrame 0,r.Test.FreezeTemporalSequences 1"
    off = dc.exec_cmds(clouds_off=True)
    assert off.startswith(base) and off.endswith(",r.VolumetricCloud 0")
