import io

from fake_sources import synthetic_scene
from fakes import FakeHttp

from camsim_scene import cli
from camsim_scene.config import parse_scene
from camsim_scene.pipeline import plan_scene


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


def test_build_with_config_keeps_an_existing_manifest_unless_replan(tmp_path, monkeypatch):
    pkg = tmp_path / "pkg"
    plan_scene(parse_scene(synthetic_scene(tmp_path / "src")), pkg, http=FakeHttp({}), out=io.StringIO())
    cache = ["--cache", str(tmp_path / "cache")]
    assert cli.main([*cache, "build", str(pkg), "-j", "1"]) == 0
    before = (pkg / "manifest.json").read_bytes()
    cfg = tmp_path / "scene.toml"
    cfg.write_text('name = "x"\nbbox = [-117.5, 33.2, -117.4, 33.3]\n')
    calls = []

    def fake_plan(plan, pkg_, http=None, **kw):
        calls.append(plan.name)
        raise cli.PlanError("planned")

    monkeypatch.setattr(cli, "plan_scene", fake_plan)
    assert cli.main([*cache, "build", str(pkg), "--config", str(cfg), "-j", "1"]) == 0
    assert calls == [] and (pkg / "manifest.json").read_bytes() == before
    assert cli.main([*cache, "build", str(pkg), "--config", str(cfg), "--replan", "-j", "1"]) == 2
    assert calls == ["x"]


def test_pack_prints_the_image_and_build_record(tmp_path, capsys, monkeypatch):
    def fake_pack(pkg, out=None):
        return tmp_path / "pkg.sqfs"

    monkeypatch.setattr(cli, "pack", fake_pack)
    assert cli.main(["pack", str(tmp_path / "pkg")]) == 0
    out = capsys.readouterr().out
    assert "pkg.sqfs" in out and "pkg.sqfs.build.json" in out
