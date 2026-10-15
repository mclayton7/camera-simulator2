"""Building snapshot partition (spec docs/superpowers/specs/2026-10-14-buildings-chunked-design.md section 4.1): every
kept row goes to the z12 chunk (buildings.CHUNK_ZOOM) of the centroid it is planned with (buildings.stage), so
planning, rasterising and GLB writing hold one chunk at a time. The chunks are one Parquet file sorted by (cx, cy, row)
in small row groups: a chunk read is a filtered scan that skips the other row groups. It is written by a POSITIONAL
JOIN of the snapshot with one key line per snapshot row (a hash join, or PARTITION_BY into ~1,400 files, ran out of
DuckDB's memory limit on socal-r5's 5.8 M rows)."""

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
CHUNKS_FILE = "chunks.parquet"
PARTITION_FORMAT = 2  # 2: one CHUNKS_FILE sorted by (cx, cy, row); 1 (unmarked) was a PARTITION_BY directory
DUCKDB_SETTINGS = ("memory_limit = '500MB'", "threads = 2")  # socal-r5: 683 MB peak RSS, 6 s
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
    """Rows [start, stop) of the snapshot -> keys_dir/<start, zero-padded>.csv: one "cx,cy" line per row, "," for a row
    that is not kept (the files in name order line up with the snapshot's rows), and the counts."""
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
    for _row, sid, wkb, height, floors, cls in rows:
        if wkb is None or sid == last:  # read_snapshot: rows without geometry skipped, the first row of an id kept
            lines.append(",\n")
            continue
        last = sid
        counts["input"] += 1
        s = B.stage(B.Footprint(sid, shapely.from_wkb(bytes(wkb)), height, floors, cls or ""), min_area_m2)
        if isinstance(s, str):
            counts[f"dropped_{s}"] += 1
            lines.append(",\n")
            continue
        cx, cy = B.chunk_of(s[1], s[2])
        chunks.add((cx, cy))
        lines.append(f"{cx},{cy}\n")
    assert len(lines) == len(rows) == stop - start
    atomic_write(Path(keys_dir) / f"{start:012d}.csv", "".join(lines).encode())
    return [{**counts, "chunks": sorted(chunks), "rss_mb": peak_rss_mb()}]


def keys_batch(batch) -> list[dict]:
    return [r for it in batch for r in partition_keys(it)]


def _noop(*_):
    pass


def read_partition(out_dir: Path) -> dict | None:
    p = Path(out_dir) / PARTITION_MARKER
    return json.loads(p.read_text()) if p.exists() else None


def usable_partition(out_dir: Path, key: str) -> dict | None:
    """The marker of a complete partition for `key` in this format under out_dir, else None."""
    prev = read_partition(out_dir)
    if (
        prev and prev["inputs"] == key and prev.get("format") == PARTITION_FORMAT
        and (not prev["chunks"] or (Path(out_dir) / CHUNKS_FILE).is_file())
    ):
        return prev
    return None


def partition(
    snapshot: Path, min_area_m2: float, out_dir: Path, key: str, jobs: int, range_rows: int = RANGE_ROWS
) -> dict:
    """Partition the snapshot under out_dir (CHUNKS_FILE), unless out_dir already holds a complete
    partition for `key`. Returns the marker: inputs, counts (input, dropped_invalid, dropped_small), chunks, peak RSS."""
    out_dir = Path(out_dir)
    prev = usable_partition(out_dir, key)
    if prev:
        return prev
    shutil.rmtree(out_dir, ignore_errors=True)
    keys_dir, tmp = out_dir / "keys", out_dir / "duckdb-tmp"
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
            for setting in DUCKDB_SETTINGS:
                con.execute(f"SET {setting}")
            con.execute(f"SET temp_directory = {_q(tmp)}")
            con.execute(
                f"COPY (SELECT s.file_row_number AS row, s.id, s.geometry, s.height, s.num_floors, s.class, k.cx, k.cy "
                f"FROM read_parquet({_q(snapshot)}, file_row_number = true) s POSITIONAL JOIN "
                f"read_csv({_q(keys_dir / '*.csv')}, header = false, auto_detect = false, delim = ',', "
                "columns = {'cx': 'INTEGER', 'cy': 'INTEGER'}) k "
                "WHERE k.cx IS NOT NULL ORDER BY k.cx, k.cy, s.file_row_number) "
                f"TO {_q(out_dir / CHUNKS_FILE)} (FORMAT parquet, ROW_GROUP_SIZE 20000)"
            )
        finally:
            con.close()
        peak = max(peak, peak_rss_mb())
    shutil.rmtree(keys_dir, ignore_errors=True)
    shutil.rmtree(tmp, ignore_errors=True)
    marker = {"inputs": key, "format": PARTITION_FORMAT, "counts": counts, "chunks": [list(c) for c in sorted(chunks)], "peak_rss_mb": peak}
    atomic_write(out_dir / PARTITION_MARKER, json.dumps(marker).encode())
    return json.loads(json.dumps(marker))


def read_chunk(out_dir: Path, cx: int, cy: int) -> list[B.Footprint]:
    """One chunk's footprints, in snapshot row order."""
    con = _connect()
    try:
        rows = con.execute(
            f"SELECT {COLS} FROM read_parquet({_q(Path(out_dir) / CHUNKS_FILE)}) WHERE cx = ? AND cy = ? ORDER BY row",
            [cx, cy],
        ).fetchall()
    finally:
        con.close()
    return [B.Footprint(sid, shapely.from_wkb(bytes(wkb)), h, f, c or "") for sid, wkb, h, f, c in rows]
