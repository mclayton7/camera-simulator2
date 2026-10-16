"""CamSim's semantic class ids (ROADMAP 4D, docs/ground-truth.md); tests/test_semantic.py holds it equal to the C++ table."""

CLASSES: dict[int, str] = {
    0: "unlabeled", 1: "sky", 2: "water", 3: "road", 4: "rail", 5: "building", 6: "built_up", 7: "tree",
    8: "low_vegetation", 9: "bare", 10: "snow_ice",
    20: "vehicle", 21: "vessel", 22: "aircraft", 23: "person", 24: "marker", 25: "other_entity",
}
