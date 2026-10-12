import pytest

from camsim_scene.roofatlas import GUTTER_PX, Placement, content_px, pack_roofs, shelf_pack, texel_ladder


def test_texel_ladder():
    assert texel_ladder(0.5) == (0.5, 0.75, 1.0, 1.5, 2.0)
    assert texel_ladder(1.0) == (1.0, 1.5, 2.0)
    assert texel_ladder(0.6) == (0.6, 0.75, 1.0, 1.5, 2.0)
    assert texel_ladder(2.0) == (2.0,)


def test_content_px_rounds_up_and_is_at_least_one():
    assert content_px(10.0, 0.5) == 20 and content_px(10.1, 0.5) == 21 and content_px(0.0, 0.5) == 1


def test_shelf_order_is_height_then_source_id():
    rects = {"b": (10, 8), "a": (10, 8), "c": (10, 20), "d": (30, 5)}
    pos = shelf_pack(rects, 64, skip_unfit=False)
    assert pos == {"c": (0, 0), "a": (10, 0), "b": (20, 0), "d": (30, 0)}


def test_shelf_wraps_to_a_new_shelf_and_reports_overflow():
    rects = {f"r{k}": (30, 30) for k in range(4)}
    assert shelf_pack(rects, 64, skip_unfit=False) == {"r0": (0, 0), "r1": (30, 0), "r2": (0, 30), "r3": (30, 30)}
    rects["r4"] = (30, 30)
    assert shelf_pack(rects, 64, skip_unfit=False) is None
    assert set(shelf_pack(rects, 64, skip_unfit=True)) == {"r0", "r1", "r2", "r3"}


def test_pack_is_deterministic_and_square_power_of_two():
    ext = {"z": (12.0, 9.0), "a": (40.0, 30.0), "m": (12.0, 9.0), "q": (5.0, 30.0)}
    p1 = pack_roofs(ext, 0.5, 2048)
    p2 = pack_roofs(dict(reversed(list(ext.items()))), 0.5, 2048)
    assert p1 == p2 and p1.texel_m == 0.5 and p1.flat == []
    assert p1.size & (p1.size - 1) == 0 and p1.size <= 2048
    a = p1.placed["a"]
    assert (a.w, a.h) == (80, 60) and a.x >= GUTTER_PX and a.y >= GUTTER_PX
    rects = sorted((pl.x - GUTTER_PX, pl.y - GUTTER_PX, pl.x + pl.w + GUTTER_PX, pl.y + pl.h + GUTTER_PX)
                   for pl in p1.placed.values())
    for k, r in enumerate(rects):  # gutter-inclusive rectangles never overlap and stay inside the atlas
        assert r[2] <= p1.size and r[3] <= p1.size
        for s in rects[k + 1:]:
            assert r[2] <= s[0] or s[2] <= r[0] or r[3] <= s[1] or s[3] <= r[1]


def test_smallest_power_of_two_that_fits():
    assert pack_roofs({"a": (10.0, 10.0)}, 0.5, 2048).size == 32  # 20 + 4 px
    empty = pack_roofs({}, 0.5, 2048)
    assert (empty.size, empty.texel_m, empty.placed, empty.flat) == (0, 0.5, {}, [])


def test_overflow_steps_the_texel_and_records_it():
    ext = {f"r{k:02d}": (300.0, 300.0) for k in range(16)}  # 604 px each at 0.5 m: 9 fit in 2048, 16 do not
    p = pack_roofs(ext, 0.5, 2048)
    assert p.texel_m == 0.75 and p.flat == [] and len(p.placed) == 16
    assert {(pl.w, pl.h) for pl in p.placed.values()} == {(400, 400)}


def test_a_roof_too_big_at_two_metres_falls_back_alone():
    ext = {"giant": (5000.0, 40.0), "small": (20.0, 20.0)}  # 2,504 px wide even at 2.0 m
    p = pack_roofs(ext, 0.5, 2048)
    assert p.flat == ["giant"] and set(p.placed) == {"small"} and p.texel_m == 0.5


def test_roofs_that_still_do_not_fit_at_two_metres_go_flat_in_order():
    ext = {f"r{k:03d}": (1000.0, 1000.0) for k in range(5)}  # 504 px at 2.0 m: 16 fit in 2048, 4 fit in 1024
    p = pack_roofs(ext, 0.5, 1024)
    assert p.texel_m == 2.0 and p.size == 1024
    assert sorted(p.placed) == ["r000", "r001", "r002", "r003"] and p.flat == ["r004"]
    assert p.placed["r000"] == Placement(GUTTER_PX, GUTTER_PX, 500, 500)


@pytest.mark.parametrize("max_px", [256, 2048])
def test_never_larger_than_max(max_px):
    ext = {f"r{k:03d}": (float(5 + k % 7), float(4 + k % 5)) for k in range(400)}
    p = pack_roofs(ext, 0.5, max_px)
    assert p.size <= max_px and len(p.placed) + len(p.flat) == 400
