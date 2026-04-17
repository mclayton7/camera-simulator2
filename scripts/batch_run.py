#!/usr/bin/env python3
"""
batch_run.py — Unattended batch runner for CamSim scenarios.

Runs N independent CamSim instances sequentially, each in its own output
directory, and collects per-run and batch-level summary JSON.

Usage:
    python3 scripts/batch_run.py --runs 10 --frames 900 --output batch_output
    python3 scripts/batch_run.py --runs 5 --seed 42 --timeout 120 --dry-run

Options:
    --runs N        Number of runs (required)
    --frames N      Target frame count per run (default: 900 = 30s @ 30fps)
    --timeout N     Wall-clock timeout per run in seconds (default: 300)
    --output DIR    Batch output root directory (default: batch_output)
    --config YAML   Base config YAML (default: deploy/camsim_config.yaml)
    --seed N        Starting seed; 0=wall-clock per run, positive=seed+run_idx (default: 0)
    --dry-run       Print what would be executed, then exit
"""
# Copyright CamSim Contributors. All Rights Reserved.

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def resolve_repo_root() -> Path:
    """Walk up from this script's directory looking for CLAUDE.md."""
    d = Path(__file__).resolve().parent
    while d != d.parent:
        if (d / "CLAUDE.md").exists():
            return d
        d = d.parent
    # Fallback: assume scripts/ is one level below repo root
    return Path(__file__).resolve().parent.parent


def _iso_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _pid_alive(pid: int) -> bool:
    """Return True if the process with *pid* is still running."""
    try:
        os.kill(pid, 0)
        return True
    except (OSError, ProcessLookupError):
        return False


# ---------------------------------------------------------------------------
# Config generation
# ---------------------------------------------------------------------------


def build_run_config(
    base_config_path: Path,
    run_dir: Path,
    run_idx: int,
    seed: int,
    target_frames: int,
) -> Path:
    """
    Read the base YAML, strip recording/ground_truth/randomization blocks,
    append per-run overrides, and write to run_dir/config.yaml.
    """
    text = base_config_path.read_text()

    # Strip existing top-level blocks that we override.
    # Match a top-level key (no leading whitespace) through the next top-level
    # key or end-of-string.  The block includes all indented continuation lines.
    for block_name in ("recording", "ground_truth", "randomization"):
        text = re.sub(
            rf"^{block_name}:.*?(?=^\S|\Z)",
            "",
            text,
            flags=re.MULTILINE | re.DOTALL,
        )

    recording_path = (run_dir / "recording.ts").as_posix()
    override = (
        f"\n# --- Per-run overrides (batch_run.py run {run_idx}) ---\n"
        f"recording:\n"
        f'  video_record_path: "{recording_path}"\n'
        f"\n"
        f"randomization:\n"
        f"  enabled: true\n"
        f"  seed: {seed}\n"
    )

    text = text.rstrip() + "\n" + override

    out_path = run_dir / "config.yaml"
    out_path.write_text(text)
    return out_path


# ---------------------------------------------------------------------------
# Launch / monitor / stop
# ---------------------------------------------------------------------------


def launch_run(
    repo_root: Path,
    config_path: Path,
    env_overrides: dict[str, str] | None = None,
) -> int:
    """
    Copy the per-run config into the project directory, then call
    ``run.sh --headless --detach --local``.  Waits up to 30 s for the PID
    file to appear and returns the UE process PID.
    """
    config_dst = repo_root / "unreal_project" / "CamSimTest" / "camsim_config.yaml"
    shutil.copy2(config_path, config_dst)

    env = os.environ.copy()
    if env_overrides:
        env.update(env_overrides)

    run_sh = repo_root / "scripts" / "run.sh"
    subprocess.run(
        [str(run_sh), "--headless", "--detach", "--local"],
        env=env,
        cwd=str(repo_root),
        check=True,
    )

    pid_file = repo_root / ".cache" / "camsim.pid"
    deadline = time.monotonic() + 30.0
    while time.monotonic() < deadline:
        if pid_file.exists():
            lines = pid_file.read_text().strip().splitlines()
            if lines:
                pid = int(lines[0])
                if _pid_alive(pid):
                    return pid
        time.sleep(0.5)

    raise RuntimeError("camsim.pid did not appear within 30 s of launch")


HEALTH_CANDIDATES_SUFFIXES = [
    Path("unreal_project") / "CamSimTest" / "Saved" / "camsim_health.json",
    Path("unreal_project") / "CamSimTest" / "camsim_health.json",
    Path(".cache") / "camsim_health.json",
    Path("camsim_health.json"),
]


def delete_stale_health_files(repo_root: Path) -> None:
    """Remove any leftover health files from a previous run."""
    for suffix in HEALTH_CANDIDATES_SUFFIXES:
        p = repo_root / suffix
        if p.exists():
            p.unlink()


def resolve_health_path(repo_root: Path, timeout: float = 30.0) -> Path | None:
    """
    Try several candidate paths for ``camsim_health.json`` and wait up to
    *timeout* seconds for one of them to appear.
    """
    candidates = [repo_root / s for s in HEALTH_CANDIDATES_SUFFIXES]
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for p in candidates:
            if p.exists():
                return p
        time.sleep(1.0)
    return None


def poll_health(
    health_path: Path,
    target_frames: int,
    timeout_sec: float,
    pid: int,
    poll_interval: float = 3.0,
) -> tuple[str, int, float]:
    """
    Poll the health JSON until we reach *target_frames*, time out, or the
    process dies.

    Returns ``(reason, final_frame, elapsed_sec)``.
    Reasons: ``"frames_reached"``, ``"timeout"``, ``"process_died"``.
    """
    start = time.monotonic()
    final_frame = 0
    while True:
        elapsed = time.monotonic() - start

        if not _pid_alive(pid):
            return ("process_died", final_frame, elapsed)

        if elapsed >= timeout_sec:
            return ("timeout", final_frame, elapsed)

        try:
            data = json.loads(health_path.read_text())
            frame = int(data.get("frame", 0))
            if frame > final_frame:
                final_frame = frame
            if final_frame >= target_frames:
                return ("frames_reached", final_frame, elapsed)
        except (json.JSONDecodeError, OSError, ValueError):
            pass  # file may be mid-write

        time.sleep(poll_interval)


def stop_run(pid: int) -> None:
    """Send SIGTERM, wait up to 15 s, then SIGKILL if still alive."""
    if not _pid_alive(pid):
        return
    try:
        os.kill(pid, signal.SIGTERM)
    except OSError:
        return
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline:
        if not _pid_alive(pid):
            return
        time.sleep(0.5)
    # Still alive — force kill
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass


# ---------------------------------------------------------------------------
# Artifact collection
# ---------------------------------------------------------------------------


def collect_run_artifacts(
    run_dir: Path,
    health_path: Path | None,
    start_time: str,
    end_time: str,
    exit_code: int,
    seed: int,
    reason: str,
    final_frame: int,
    elapsed_sec: float,
) -> dict:
    """
    Copy the health JSON into *run_dir* and write a ``summary.json``.
    Returns the summary dict.
    """
    if health_path and health_path.exists():
        shutil.copy2(health_path, run_dir / "camsim_health.json")

    summary = {
        "run_index": int(run_dir.name.replace("run_", "")),
        "start_time_iso": start_time,
        "end_time_iso": end_time,
        "elapsed_sec": round(elapsed_sec, 1),
        "exit_code": exit_code,
        "frames_rendered": final_frame,
        "seed_used": seed,
        "completion_reason": reason,
    }

    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--runs", type=int, required=True, help="Number of runs")
    ap.add_argument(
        "--frames",
        type=int,
        default=900,
        help="Target frame count per run (default: 900)",
    )
    ap.add_argument(
        "--timeout",
        type=int,
        default=300,
        help="Wall-clock timeout per run in seconds (default: 300)",
    )
    ap.add_argument(
        "--output",
        default="batch_output",
        help="Batch output root directory (default: batch_output)",
    )
    ap.add_argument(
        "--config",
        default=None,
        help="Base config YAML (default: deploy/camsim_config.yaml)",
    )
    ap.add_argument(
        "--seed",
        type=int,
        default=0,
        help="Starting seed; 0=wall-clock per run (default: 0)",
    )
    ap.add_argument(
        "--dry-run", action="store_true", help="Print what would be executed, then exit"
    )
    args = ap.parse_args()

    repo_root = resolve_repo_root()

    base_config = (
        Path(args.config)
        if args.config
        else repo_root / "deploy" / "camsim_config.yaml"
    )
    if not base_config.exists():
        print(f"[ERROR] Base config not found: {base_config}", file=sys.stderr)
        sys.exit(1)

    output_root = Path(args.output)
    output_root.mkdir(parents=True, exist_ok=True)

    batch_start = _iso_now()
    run_summaries: list[dict] = []
    completed = 0
    timed_out = 0
    failed = 0

    print("==> CamSim Batch Runner")
    print(f"    Runs:        {args.runs}")
    print(f"    Frames/run:  {args.frames}")
    print(f"    Timeout/run: {args.timeout}s")
    print(f"    Output:      {output_root.resolve()}")
    print(f"    Base config: {base_config}")
    print(f"    Seed:        {'wall-clock' if args.seed == 0 else args.seed}")
    print()

    if args.dry_run:
        for i in range(args.runs):
            seed = 0 if args.seed == 0 else args.seed + i
            run_dir = output_root / f"run_{i:04d}"
            print(
                f"  [DRY-RUN] run {i}: dir={run_dir}  seed={seed}  frames={args.frames}"
            )
        print("\n  --dry-run: nothing executed.")
        return

    current_pid: int | None = None

    try:
        for i in range(args.runs):
            seed = 0 if args.seed == 0 else args.seed + i
            run_dir = output_root / f"run_{i:04d}"
            run_dir.mkdir(parents=True, exist_ok=True)

            print(f"--- Run {i}/{args.runs - 1}  seed={seed}  dir={run_dir} ---")

            reason = "unknown"
            final_frame = 0
            elapsed_sec = 0.0
            exit_code = 1
            start_time = _iso_now()
            health_path: Path | None = None

            try:
                config_path = build_run_config(
                    base_config,
                    run_dir,
                    i,
                    seed,
                    args.frames,
                )
                print(f"    Config written: {config_path}")

                delete_stale_health_files(repo_root)
                pid = launch_run(repo_root, config_path)
                current_pid = pid
                print(f"    Launched PID {pid}")

                health_path = resolve_health_path(repo_root)
                if health_path is None:
                    print("    [WARN] Health file never appeared")
                    reason = "health_never_appeared"
                else:
                    print(f"    Health: {health_path}")
                    reason, final_frame, elapsed_sec = poll_health(
                        health_path,
                        args.frames,
                        args.timeout,
                        pid,
                    )
                    print(
                        f"    Result: {reason}  frames={final_frame}  elapsed={elapsed_sec:.1f}s"
                    )

                if reason == "frames_reached":
                    exit_code = 0
                    completed += 1
                elif reason == "timeout":
                    exit_code = 1
                    timed_out += 1
                else:
                    exit_code = 1
                    failed += 1

            except Exception as exc:
                print(f"    [ERROR] {exc}")
                reason = "error"
                failed += 1

            finally:
                end_time = _iso_now()
                if current_pid is not None:
                    stop_run(current_pid)
                    current_pid = None

                summary = collect_run_artifacts(
                    run_dir,
                    health_path,
                    start_time,
                    end_time,
                    exit_code,
                    seed,
                    reason,
                    final_frame,
                    elapsed_sec,
                )
                run_summaries.append(summary)

            print()

    except KeyboardInterrupt:
        print("\n[INTERRUPTED] Stopping current run …")
        if current_pid is not None:
            stop_run(current_pid)
        # Write partial summary and exit 130
        _write_batch_summary(
            output_root, batch_start, run_summaries, completed, timed_out, failed
        )
        sys.exit(130)

    _write_batch_summary(
        output_root, batch_start, run_summaries, completed, timed_out, failed
    )

    print(
        f"==> Batch complete: {completed} completed, {timed_out} timed out, {failed} failed"
    )


def _write_batch_summary(
    output_root: Path,
    batch_start: str,
    run_summaries: list[dict],
    completed: int,
    timed_out: int,
    failed: int,
) -> None:
    batch_summary = {
        "batch_start_iso": batch_start,
        "batch_end_iso": _iso_now(),
        "total_runs": len(run_summaries),
        "completed": completed,
        "timed_out": timed_out,
        "failed": failed,
        "runs": run_summaries,
    }
    path = output_root / "batch_summary.json"
    path.write_text(json.dumps(batch_summary, indent=2) + "\n")
    print(f"    Batch summary: {path}")


if __name__ == "__main__":
    main()
