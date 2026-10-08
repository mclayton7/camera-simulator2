import json

from camsim_scene import fsutil
from camsim_scene.manifest import (
    AssetRecord,
    Manifest,
    SourceRecord,
    canonical_json,
    package_files,
    read_hashes,
    write_hashes,
)


def sample_manifest() -> Manifest:
    src = SourceRecord(
        id="dep3_13",
        adapter="dep3_13",
        dataset="USGS 3DEP 1/3 arc-second DEM",
        version="20260915",
        licence="LicenseRef-PublicDomain-USGov",
        attribution="U.S. Geological Survey",
        options={},
        assets=[
            AssetRecord(
                id="USGS_13_n34w118_20260915", url="https://x/y.tif", sha256="ab" * 32, size=10, group="dep3_13"
            )
        ],
    )
    return Manifest(
        name="t",
        bbox=[0.0, 0.0, 1.0, 1.0],
        seed=0,
        regions=[],
        tiling={"scheme": "tms"},
        layers={"terrain": {"grid": 257}, "imagery": {"jpeg_quality": 85}, "landcover": {}},
        datum={"grids": {}, "datums": {}},
        sources=[src],
        licence_allow=["CC-BY-4.0", "LicenseRef-PublicDomain-USGov"],
        tool={"name": "camsim-scene", "version": "0.1.0"},
    )


def test_canonical_json_is_key_order_independent():
    a = canonical_json({"b": 1, "a": [1.5, {"d": 0.1, "c": None}]})
    assert a == canonical_json({"a": [1.5, {"c": None, "d": 0.1}], "b": 1})


def test_manifest_round_trip_and_fetched(tmp_path):
    m = sample_manifest()
    m.write(tmp_path / "manifest.json")
    back = Manifest.load(tmp_path / "manifest.json")
    assert back.to_dict() == m.to_dict() and back.dumps() == m.dumps()
    assert back.source("dep3_13").assets[0].group == "dep3_13"
    assert back.is_fetched()
    back.sources[0].assets[0].sha256 = None
    assert not back.is_fetched()
    assert json.loads(m.dumps())["schema_version"] == 1


def test_settings_hash_changes_only_for_the_changed_layer():
    a, b = sample_manifest(), sample_manifest()
    b.layers["imagery"]["jpeg_quality"] = 90
    assert a.layer_settings_hash("terrain") == b.layer_settings_hash("terrain")
    assert a.layer_settings_hash("imagery") != b.layer_settings_hash("imagery")
    b.hashes_sha256 = "ff" * 32  # output, not an input
    assert a.layer_settings_hash("terrain") == b.layer_settings_hash("terrain")


def test_hashes_txt_is_sorted_and_excludes_state_and_meta(tmp_path):
    for rel in [
        "terrain/1/0/0.terrain",
        "terrain/10/0/0.terrain",
        "terrain/1.json",
        "ATTRIBUTION.txt",
        "manifest.json",
        "build.json",
        ".state/terrain/1/0/0",
        "imagery/0/0/0.jpg",
    ]:
        fsutil.atomic_write(tmp_path / rel, rel.encode())
    files = list(package_files(tmp_path))
    assert files == sorted(files)
    assert files == [
        "ATTRIBUTION.txt",
        "imagery/0/0/0.jpg",
        "terrain/1.json",
        "terrain/1/0/0.terrain",
        "terrain/10/0/0.terrain",
    ]
    digest = write_hashes(tmp_path, known=lambda rel: "00" * 32 if rel.endswith(".terrain") else None)
    assert digest == fsutil.sha256_file(tmp_path / "hashes.txt")
    h = read_hashes(tmp_path)
    assert h["terrain/1/0/0.terrain"] == "00" * 32
    assert h["ATTRIBUTION.txt"] == fsutil.sha256_bytes(b"ATTRIBUTION.txt")
    assert "manifest.json" not in h and "hashes.txt" not in h
