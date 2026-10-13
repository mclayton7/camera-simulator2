"""install_entries.py: prepare_tree.py output merged into library.json by name (REALISM R4)."""

from vegetation import install_entries as ie


def test_entries_replace_by_name_in_place_and_append_new():
    lib = {"format": "camsim-vegetation-2", "types": [{"name": "a", "v": 1}, {"name": "b", "v": 1}, {"name": "c", "v": 1}]}
    text = 'noise\n{\n  "name": "b",\n  "v": 2\n}\n{"name": "d", "v": 1}\n'
    out = ie.merge(lib, ie.read_entries(text))
    assert [t["name"] for t in out["types"]] == ["a", "b", "c", "d"]       # order kept: index 0 stays tree_broadleaf_a
    assert out["types"][1]["v"] == 2 and out["types"][0]["v"] == 1
