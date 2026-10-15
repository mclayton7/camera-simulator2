"""Building snapshot partition (spec docs/superpowers/specs/2026-10-14-buildings-chunked-design.md section 4.1): every
kept row goes to the z12 chunk (buildings.CHUNK_ZOOM) of the centroid it is planned with (buildings.stage), one
Parquet file per chunk, so planning, rasterising and GLB writing hold one chunk at a time."""

from __future__ import annotations

import json
import shutil
from pathlib import Path

import shapely

from . import buildings as B
from .engine import peak_rss_mb, run_pool
from .fsutil import atomic_write

RANGE_ROWS = 100_000
PARTITION_MARKER = "partition.json"
COLS = "id, geometry, height, num_floors, class"


def _q(text) -> str:
    return "'" + str(text).replace("'", "''") + "'"


def _connect():
    from .sources.overture_buildings import connect

    return connect()


def _already_kept(con, snapshot: str, start: int, sid: str) -> bool:
    """Whether an earlier row of `sid` (rows are sorted by id) had a geometry: read_snapshot kept that one."""
    if start == 0:
        return False
    n = con.execute(
        f"SELECT count(*) FROM read_parquet({_q(snapshot)}, file_row_number = true) "
        "WHERE id = ? AND file_row_number < ? AND geometry IS NOT NULL",
        [sid, start],
    ).fetchone()[0]
    return n > 0


def partition_keys(item) -> list[dict]:
    """Rows [start, stop) of the snapshot -> keys_dir/<start>.csv (row,cx,cy of every kept row) and the counts."""
    snapshot, start, stop, min_area_m2, keys_dir = item
    con = _connect()
    try:
        rows = con.execute(
            f"SELECT file_row_number, {COLS} FROM read_parquet({_q(snapshot)}, file_row_number = true) "
            "WHERE file_row_number >= ? AND file_row_number < ? ORDER BY file_row_number",
            [start, stop],
        ).fetchall()
        last = rows[0][1] if rows and _already_kept(con, snapshot, start, rows[0][1]) else None
    finally:
        con.close()
    counts = {"input": 0, "dropped_invalid": 0, "dropped_small": 0}
    lines, chunks = [], set()
    for row, sid, wkb, height, floors, cls in rows:
        if wkb is None or sid == last:  # read_snapshot: rows without geometry skipped, the first row of an id kept
            continue
        last = sid
        counts["input"] += 1
        s = B.stage(B.Footprint(sid, shapely.from_wkb(bytes(wkb)), height, floors, cls or ""), min_area_m2)
        if isinstance(s, str):
            counts[f"dropped_{s}"] += 1
            continue
        cx, cy = B.chunk_of(s[1], s[2])
        chunks.add((cx, cy))
        lines.append(f"{row},{cx},{cy}\n")
    if lines:  # DuckDB's read_csv rejects an empty file
        atomic_write(Path(keys_dir) / f"{start}.csv", "".join(lines).encode())
    return [{**counts, "chunks": sorted(chunks), "rss_mb": peak_rss_mb()}]


def keys_batch(batch) -> list[dict]:
    return [r for it in batch for r in partition_keys(it)]


def _noop(*_):
    pass


def read_partition(out_dir: Path) -> dict | None:
    p = Path(out_dir) / PARTITION_MARKER
    return json.loads(p.read_text()) if p.exists() else None


def partition(
    snapshot: Path, min_area_m2: float, out_dir: Path, key: str, jobs: int, range_rows: int = RANGE_ROWS
) -> dict:
    """Partition the snapshot under out_dir (chunks/cx=<x>/cy=<y>/*.parquet), unless out_dir already holds a complete
    partition for `key`. Returns the marker: inputs, counts (input, dropped_invalid, dropped_small), chunks, peak RSS."""
    out_dir = Path(out_dir)
    prev = read_partition(out_dir)
    if prev and prev["inputs"] == key:
        return prev
    shutil.rmtree(out_dir, ignore_errors=True)
    keys_dir, chunk_dir, tmp = out_dir / "keys", out_dir / "chunks", out_dir / "duckdb-tmp"
    keys_dir.mkdir(parents=True)
    con = _connect()
    try:
        n = con.execute(f"SELECT count(*) FROM read_parquet({_q(snapshot)})").fetchone()[0]
    finally:
        con.close()
    items = [(str(snapshot), a, min(a + range_rows, n), float(min_area_m2), str(keys_dir)) for a in range(0, n, range_rows)]
    counts = {"input": 0, "dropped_invalid": 0, "dropped_small": 0}
    chunks, peak = set(), peak_rss_mb()

    def on_result(r):
        nonlocal peak
        for k in counts:
            counts[k] += r[k]
        chunks.update(tuple(c) for c in r["chunks"])
        peak = max(peak, r["rss_mb"])

    run_pool(keys_batch, [[[it] for it in items]], jobs, _noop, (), on_result)
    if chunks:
        con = _connect()
        try:
            con.execute("SET memory_limit = '1GB'")
            con.execute(f"SET temp_directory = {_q(tmp)}")
            con.execute(
                f"COPY (SELECT s.file_row_number AS row, s.id, s.geometry, s.height, s.num_floors, s.class, k.cx, k.cy "
                f"FROM read_parquet({_q(snapshot)}, file_row_number = true) s "
                f"JOIN read_csv({_q(keys_dir / '*.csv')}, header = false, auto_detect = false, delim = ',', "
                "columns = {'row': 'BIGINT', 'cx': 'INTEGER', 'cy': 'INTEGER'}) k ON s.file_row_number = k.row) "
                f"TO {_q(chunk_dir)} (FORMAT parquet, PARTITION_BY (cx, cy))"
            )
        finally:
            con.close()
        peak = max(peak, peak_rss_mb())
    shutil.rmtree(keys_dir, ignore_errors=True)
    shutil.rmtree(tmp, ignore_errors=True)
    marker = {"inputs": key, "counts": counts, "chunks": [list(c) for c in sorted(chunks)], "peak_rss_mb": peak}
    atomic_write(out_dir / PARTITION_MARKER, json.dumps(marker).encode())
    return json.loads(json.dumps(marker))


def read_chunk(out_dir: Path, cx: int, cy: int) -> list[B.Footprint]:
    """One chunk's footprints, in snapshot row order."""
    files = Path(out_dir) / "chunks" / f"cx={cx}" / f"cy={cy}" / "*.parquet"
    con = _connect()
    try:
        rows = con.execute(f"SELECT {COLS} FROM read_parquet({_q(files)}) ORDER BY row").fetchall()
    finally:
        con.close()
    return [B.Footprint(sid, shapely.from_wkb(bytes(wkb)), h, f, c or "") for sid, wkb, h, f, c in rows]
