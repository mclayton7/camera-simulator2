import io

import pytest
from fake_sources import synthetic_scene
from fakes import FakeHttp

from camsim_scene import cli
from camsim_scene.cache import Cache
from camsim_scene.config import parse_scene
from camsim_scene.engine import BuildLock, BuildLocked
from camsim_scene.pipeline import fetch_scene, plan_scene


def test_build_verify_attribution_round_trip(tmp_path, capsys):
    pkg = tmp_path / "pkg"
    plan_scene(parse_scene(synthetic_scene(tmp_path / "src")), pkg, http=FakeHttp({}), out=io.StringIO())
    cache = ["--cache", str(tmp_path / "cache")]
    assert cli.main([*cache, "build", str(pkg), "-j", "1"]) == 0
    assert cli.main([*cache, "verify", str(pkg)]) == 0
    assert cli.main([*cache, "attribution", str(pkg)]) == 0
    assert "synthetic test data" in capsys.readouterr().out


def test_build_without_manifest_or_config_is_a_usage_error(tmp_path, capsys):
    assert cli.main(["--cache", str(tmp_path / "c"), "build", str(tmp_path / "nothing")]) == 2
    assert "manifest.json" in capsys.readouterr().err


def test_bbox_across_the_antimeridian_is_rejected_before_any_network(tmp_path, capsys, monkeypatch):
    monkeypatch.chdir(tmp_path)
    assert cli.main(["plan", str(tmp_path / "p"), "--bbox", "177", "-19", "-179", "-16", "--name", "fiji"]) == 2
    assert "antimeridian" in capsys.readouterr().err


def test_verify_failure_exit_code(tmp_path):
    pkg = tmp_path / "pkg"
    plan_scene(parse_scene(synthetic_scene(tmp_path / "src")), pkg, http=FakeHttp({}), out=io.StringIO())
    cache = ["--cache", str(tmp_path / "cache")]
    cli.main([*cache, "build", str(pkg), "-j", "1"])
    (pkg / "ATTRIBUTION.txt").write_text("tampered\n")
    assert cli.main([*cache, "verify", str(pkg)]) == 1


def test_build_with_config_keeps_an_existing_manifest_unless_replan(tmp_path, monkeypatch, capsys):
    scene = synthetic_scene(tmp_path / "src")
    pkg = tmp_path / "pkg"
    plan_scene(parse_scene(scene), pkg, http=FakeHttp({}), out=io.StringIO())
    cache = ["--cache", str(tmp_path / "cache")]
    assert cli.main([*cache, "build", str(pkg), "-j", "1"]) == 0
    before = (pkg / "manifest.json").read_bytes()
    cfg = tmp_path / "scene.toml"
    cfg.write_text("# loaded through the patched load_scene\n")
    loaded = {"scene": scene}
    monkeypatch.setattr(cli, "load_scene", lambda path: parse_scene(loaded["scene"]))
    calls = []

    def fake_plan(plan, pkg_, http=None, **kw):
        calls.append(plan.name)
        raise cli.PlanError("planned")

    monkeypatch.setattr(cli, "plan_scene", fake_plan)
    # the same scene: kept, not re-planned
    assert cli.main([*cache, "build", str(pkg), "--config", str(cfg), "-j", "1"]) == 0
    assert calls == [] and (pkg / "manifest.json").read_bytes() == before
    # an edited scene without --replan: refused, naming --replan
    loaded["scene"] = {**scene, "ring_km": 40}
    capsys.readouterr()
    assert cli.main([*cache, "build", str(pkg), "--config", str(cfg), "-j", "1"]) == 2
    err = capsys.readouterr().err
    assert "--replan" in err and "regions" in err and calls == []
    assert (pkg / "manifest.json").read_bytes() == before
    # an extra --allow without --replan: refused too
    loaded["scene"] = scene
    assert cli.main([*cache, "build", str(pkg), "--config", str(cfg), "--allow", "ODbL-1.0", "-j", "1"]) == 2
    assert "licence_allow" in capsys.readouterr().err and calls == []
    assert cli.main([*cache, "build", str(pkg), "--config", str(cfg), "--replan", "-j", "1"]) == 2
    assert calls == ["synthetic"]


def test_allow_name_profile_without_a_scene_are_rejected(tmp_path, capsys):
    pkg = tmp_path / "pkg"
    plan_scene(parse_scene(synthetic_scene(tmp_path / "src")), pkg, http=FakeHttp({}), out=io.StringIO())
    before = (pkg / "manifest.json").read_bytes()
    cache = ["--cache", str(tmp_path / "cache")]
    for extra in (["--allow", "ODbL-1.0"], ["--name", "other"], ["--profile", "preview"]):
        assert cli.main([*cache, "build", str(pkg), *extra, "-j", "1"]) == 2, extra
        assert extra[0] in capsys.readouterr().err
    assert (pkg / "manifest.json").read_bytes() == before


def test_bbox_scene_file_is_written_beside_the_package(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    (tmp_path / "out").mkdir()
    monkeypatch.setattr(cli, "plan_scene", lambda plan, pkg, http=None, **kw: None)
    assert cli.main(["plan", str(tmp_path / "out/site"), "--bbox", "10", "10", "10.5", "10.5"]) == 0
    assert (tmp_path / "out/site.scene.toml").exists() and not (tmp_path / "site.scene.toml").exists()


def test_bbox_conflicting_with_an_existing_scene_file_is_rejected(tmp_path, monkeypatch, capsys):
    calls = []
    monkeypatch.setattr(cli, "plan_scene", lambda plan, pkg, http=None, **kw: calls.append(plan))
    pkg = str(tmp_path / "site")
    assert cli.main(["plan", pkg, "--bbox", "10", "10", "10.5", "10.5"]) == 0
    scene_file = tmp_path / "site.scene.toml"
    text = scene_file.read_text()
    for args in (
        ["--bbox", "10", "10", "10.2", "10.2"],
        ["--bbox", "10", "10", "10.5", "10.5", "--profile", "preview"],
        ["--bbox", "10", "10", "10.5", "10.5", "--name", "other"],
    ):
        if "--name" in args:
            (tmp_path / "other.scene.toml").write_text(text)  # another package's file under that name
        capsys.readouterr()
        assert cli.main(["plan", pkg, *args]) == 2, args
        assert "scene.toml" in capsys.readouterr().err
    assert len(calls) == 1 and scene_file.read_text() == text
    assert cli.main(["plan", pkg, "--bbox", "10", "10", "10.5", "10.5"]) == 0  # same arguments: reused
    assert len(calls) == 2


def test_unwritable_scene_file_location_is_a_usage_error(tmp_path, capsys):
    ro = tmp_path / "ro"
    ro.mkdir()
    ro.chmod(0o555)
    try:
        assert cli.main(["plan", str(ro / "site"), "--bbox", "10", "10", "10.5", "10.5"]) == 2
    finally:
        ro.chmod(0o755)
    err = capsys.readouterr().err
    assert "site.scene.toml" in err and "Traceback" not in err


def test_plan_fetch_and_replan_refuse_while_a_build_holds_the_lock(tmp_path):
    scene = synthetic_scene(tmp_path / "src")
    pkg = tmp_path / "pkg"
    plan_scene(parse_scene(scene), pkg, http=FakeHttp({}), out=io.StringIO())
    before = (pkg / "manifest.json").read_bytes()
    cache = ["--cache", str(tmp_path / "cache")]
    cfg = tmp_path / "scene.toml"
    cfg.write_text("x")
    with BuildLock(pkg):
        with pytest.raises(BuildLocked):
            plan_scene(parse_scene({**scene, "ring_km": 40}), pkg, http=FakeHttp({}), out=io.StringIO())
        with pytest.raises(BuildLocked):
            fetch_scene(pkg, Cache(tmp_path / "cache"))
        assert cli.main([*cache, "fetch", str(pkg)]) == 2
        with pytest.MonkeyPatch.context() as mp:
            mp.setattr(cli, "load_scene", lambda path: parse_scene({**scene, "ring_km": 40}))
            assert cli.main([*cache, "build", str(pkg), "--config", str(cfg), "--replan", "-j", "1"]) == 2
    assert (pkg / "manifest.json").read_bytes() == before


def test_pack_prints_the_image_and_build_record(tmp_path, capsys, monkeypatch):
    def fake_pack(pkg, out=None):
        return tmp_path / "pkg.sqfs"

    monkeypatch.setattr(cli, "pack", fake_pack)
    assert cli.main(["pack", str(tmp_path / "pkg")]) == 0
    out = capsys.readouterr().out
    assert "pkg.sqfs" in out and "pkg.sqfs.build.json" in out
