#!/usr/bin/env python3
"""Parse a UE5 Automation Test report (`index.json`) for CI consumption.

Reads the JSON written by `UnrealEditor ... -ReportExportPath=<dir>`, prints a
single summary line, emits a `::error::` annotation per failing test for GitHub
Actions, and appends a markdown summary table to `$GITHUB_STEP_SUMMARY` when
that variable is set.

Exit codes:
    0 — every test succeeded (warnings allowed)
    1 — at least one test failed (or ended `InProcess`, or has `errors > 0`)
    2 — the report file is missing (harness likely crashed before writing it)

The file is read with `encoding="utf-8-sig"` because UE5 writes a UTF-8 BOM.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any


def fmt_seconds(value: float) -> str:
    if value < 60.0:
        return f"{value:.1f}s"
    return f"{int(value // 60)}m{int(value % 60)}s"


def load_report(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8-sig") as fp:
        return json.load(fp)


def summarize(report: dict[str, Any]) -> tuple[dict[str, Any], list[dict], list[dict]]:
    """Reduce a parsed report into (counts, failed_tests, warning_tests)."""
    counts: dict[str, Any] = {
        "succeeded": int(report.get("succeeded", 0)),
        "succeededWithWarnings": int(report.get("succeededWithWarnings", 0)),
        "failed": int(report.get("failed", 0)),
        "notRun": int(report.get("notRun", 0)),
        "inProcess": int(report.get("inProcess", 0)),
        "totalDuration": float(report.get("totalDuration", 0.0)),
    }
    counts["total"] = (
        counts["succeeded"]
        + counts["succeededWithWarnings"]
        + counts["failed"]
        + counts["notRun"]
        + counts["inProcess"]
    )

    failed: list[dict] = []
    warned: list[dict] = []
    for test in report.get("tests") or []:
        state = test.get("state", "")
        errors_n = int(test.get("errors") or 0)
        warnings_n = int(test.get("warnings") or 0)
        if state == "Fail" or state == "InProcess" or errors_n > 0:
            failed.append(test)
        elif state == "Success" and warnings_n > 0:
            warned.append(test)
    return counts, failed, warned


def collect_messages(test: dict, kind: str) -> list[str]:
    """Pull `event.message` strings of a given kind ("error" / "warning") from `entries`."""
    out: list[str] = []
    for entry in test.get("entries") or []:
        event = entry.get("event") or {}
        if (event.get("type") or "").lower() == kind:
            msg = event.get("message")
            if msg:
                out.append(str(msg))
    return out


def emit_gha_annotation(test: dict) -> None:
    path = test.get("fullTestPath") or test.get("testDisplayName") or "<unknown>"
    messages = collect_messages(test, "error") or [test.get("state", "Fail")]
    joined = " | ".join(messages)
    safe = joined.replace("\r", " ").replace("\n", "%0A")
    print(f"::error title=UE5 test failed: {path}::{safe}", flush=True)


def write_step_summary(
    counts: dict[str, Any], failed: list[dict], warned: list[dict], out
) -> None:
    out.write("# UE5 Automation Report\n\n")
    out.write("| State | Count |\n|---|---|\n")
    out.write(f"| Succeeded | {counts['succeeded']} |\n")
    out.write(f"| Succeeded (with warnings) | {counts['succeededWithWarnings']} |\n")
    out.write(f"| Failed | {counts['failed']} |\n")
    out.write(f"| Not run | {counts['notRun']} |\n")
    out.write(f"| In process | {counts['inProcess']} |\n")
    out.write(f"| **Total** | **{counts['total']}** |\n\n")
    out.write(f"_Total wall time: {fmt_seconds(counts['totalDuration'])}_\n\n")

    if failed:
        out.write("## Failures\n\n")
        for test in failed:
            path = (
                test.get("fullTestPath") or test.get("testDisplayName") or "<unknown>"
            )
            out.write(f"### `{path}`\n\n")
            for msg in collect_messages(test, "error"):
                out.write(f"- {msg}\n")
            out.write("\n")

    if warned:
        out.write(f"<details><summary>Warnings ({len(warned)} tests)</summary>\n\n")
        for test in warned:
            path = (
                test.get("fullTestPath") or test.get("testDisplayName") or "<unknown>"
            )
            out.write(f"- `{path}`\n")
            for msg in collect_messages(test, "warning"):
                out.write(f"  - {msg}\n")
        out.write("\n</details>\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "report",
        help="Path to index.json, or to the directory that contains it",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Skip ::error:: annotations and $GITHUB_STEP_SUMMARY output",
    )
    args = parser.parse_args(argv)

    report_path = Path(args.report)
    if report_path.is_dir():
        report_path = report_path / "index.json"
    if not report_path.exists():
        print(
            f"::error::Automation report missing at {report_path} "
            "— harness likely crashed before writing it",
            flush=True,
        )
        return 2

    report = load_report(report_path)
    counts, failed, warned = summarize(report)

    print(
        "automation: "
        f"succeeded={counts['succeeded']} "
        f"warnings={counts['succeededWithWarnings']} "
        f"failed={counts['failed']} "
        f"notRun={counts['notRun']} "
        f"inProcess={counts['inProcess']} "
        f"total={counts['total']} "
        f"duration={fmt_seconds(counts['totalDuration'])}",
        flush=True,
    )

    if not args.quiet:
        for test in failed:
            emit_gha_annotation(test)
        summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
        if summary_path:
            with open(summary_path, "a", encoding="utf-8") as fp:
                write_step_summary(counts, failed, warned, fp)

    return 0 if not failed and counts["failed"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
