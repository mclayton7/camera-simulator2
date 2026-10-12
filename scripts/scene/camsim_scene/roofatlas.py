"""Roof atlas packing (REALISM R3b, spec section 2 "Packing"). Each roof's axis-aligned box in the tile's local ENU
metres becomes one rectangle of content pixels plus GUTTER_PX on each side; a deterministic shelf packer places them,
ordered by rectangle height in pixels descending, then source_id ascending (unique, so no ties). The atlas is the
smallest square power of two that holds them, at most max_px; if none does, the texel steps up the ladder. A roof that
cannot fit even alone at the last step is flat from the start (so one giant roof does not coarsen the whole tile); the
ones that still do not fit at the last step are flat too, skipped in packing order. Pure; no I/O."""

from __future__ import annotations

import math
from dataclasses import dataclass, field

GUTTER_PX = 2
TEXEL_STEPS_M = (0.5, 0.75, 1.0, 1.5, 2.0)


@dataclass(frozen=True)
class Placement:
    x: int  # content rectangle, atlas pixels (row 0 = top = north); its gutter is GUTTER_PX outside it
    y: int
    w: int
    h: int


@dataclass
class Packing:
    size: int  # atlas side in pixels; 0 = nothing placed
    texel_m: float
    placed: dict[str, Placement] = field(default_factory=dict)
    flat: list[str] = field(default_factory=list)  # sorted source ids that keep the flat roof material


def texel_ladder(start_m: float) -> tuple[float, ...]:
    return (float(start_m),) + tuple(s for s in TEXEL_STEPS_M if s > start_m + 1e-9)


def content_px(extent_m: float, texel_m: float) -> int:
    return max(1, math.ceil(extent_m / texel_m - 1e-9))


def _rects(extents: dict[str, tuple[float, float]], texel_m: float) -> dict[str, tuple[int, int]]:
    g = 2 * GUTTER_PX
    return {k: (content_px(w, texel_m) + g, content_px(h, texel_m) + g) for k, (w, h) in extents.items()}


def shelf_pack(rects: dict[str, tuple[int, int]], size: int, skip_unfit: bool) -> dict[str, tuple[int, int]] | None:
    x = y = shelf_h = 0
    out: dict[str, tuple[int, int]] = {}
    for k in sorted(rects, key=lambda k: (-rects[k][1], k)):
        w, h = rects[k]
        if w > size or h > size:
            if skip_unfit:
                continue
            return None
        if x + w > size:
            y, x, shelf_h = y + shelf_h, 0, 0
        if y + h > size:
            if skip_unfit:
                continue
            return None
        out[k] = (x, y)
        x += w
        shelf_h = max(shelf_h, h)
    return out


def _pow2_at_least(n: int) -> int:
    return 1 << max(0, math.ceil(math.log2(max(1, n))))


def _placements(rects: dict[str, tuple[int, int]], pos: dict[str, tuple[int, int]]) -> dict[str, Placement]:
    g = GUTTER_PX
    return {k: Placement(x + g, y + g, rects[k][0] - 2 * g, rects[k][1] - 2 * g) for k, (x, y) in sorted(pos.items())}


def pack_roofs(extents: dict[str, tuple[float, float]], start_texel_m: float, max_px: int) -> Packing:
    ladder = texel_ladder(start_texel_m)
    last = _rects(extents, ladder[-1])
    flat = sorted(k for k, (w, h) in last.items() if w > max_px or h > max_px)
    todo = {k: v for k, v in extents.items() if k not in flat}
    if not todo:
        return Packing(0, ladder[0], {}, flat)
    for t in ladder:
        rects = _rects(todo, t)
        area = sum(w * h for w, h in rects.values())
        size = _pow2_at_least(max(max(max(r) for r in rects.values()), math.ceil(math.sqrt(area))))
        while size <= max_px:
            pos = shelf_pack(rects, size, skip_unfit=False)
            if pos is not None:
                return Packing(size, t, _placements(rects, pos), flat)
            size *= 2
    t = ladder[-1]
    rects = _rects(todo, t)
    pos = shelf_pack(rects, max_px, skip_unfit=True)
    flat = sorted(set(flat) | (set(todo) - set(pos)))
    if not pos:
        return Packing(0, t, {}, flat)
    return Packing(max_px, t, _placements(rects, pos), flat)
