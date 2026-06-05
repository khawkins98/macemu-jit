#!/usr/bin/env python3
"""Benchmark entry point (ROADMAP A5 / P2).

Boots the Mac OS 9 + Speedometer disk (Speedometer auto-launches from Startup Items), drives the
full Speedometer suite over VNC, captures the results screenshot + the emulator log, and shuts
down cleanly via the host->guest hook. Requires a logged-in macOS GUI session (SDL window).
"""
import sys
import os
import shutil
import subprocess
import time
import tempfile
from pathlib import Path

from sse2e import bench_export, config, disk, runner, scenario

HERE = Path(__file__).parent
EMULATOR = HERE.parent / "src" / "Unix" / "SheepShaver"
VNCPORT = 5950


def _run_once(*, assets, artifacts: Path, shutdown_timeout: float):
    """One full boot -> benchmark -> shutdown -> extract -> archive cycle.

    Returns (ok, report): ok is the lifecycle pass/fail; report is the parsed Report (or None
    if extraction was skipped). Each run uses a fresh pristine clonefile copy, cleaned up after.
    """
    # Pre-flight: kill+reap strays and confirm the disk master is free (a stray holding it would
    # boot this run to the "?" no-boot-disk icon).
    problem = runner.preflight(assets.disk)
    if problem:
        print(f"FAIL: preflight — {problem}")
        return False, None
    work = Path(tempfile.mkdtemp(prefix="ss-e2e-bench-"))
    try:
        run_disk = disk.copy_pristine(Path(assets.disk), work)  # instant APFS clonefile
        prefs = disk.render_prefs(
            HERE / "config" / "test.prefs.template", work / "bench.prefs",
            rom=assets.rom, disk=str(run_disk), vncport=VNCPORT)
        res = scenario.run_benchmark(
            emulator=str(EMULATOR), prefs=str(prefs), vncport=VNCPORT, artifact_dir=artifacts,
            # Shutdown allows for the Mac OS 9 disk flush (≈20–40s) + quit-to-Finder; 60s leaves
            # margin without masking a genuine hang. Override via the env var (e.g. to inspect a
            # stuck shutdown on-screen before the harness force-kills it).
            shutdown_timeout=shutdown_timeout)
        print(f"{'PASS' if res.ok else 'FAIL'}: {res.reason}")
        if res.result_image:
            print(f"  results image: {res.result_image}")
        print(f"  emulator log:  {artifacts / 'benchmark-emulator.log'}")
        if not res.ok:
            return False, None

        # Best-effort history export. Collect+report only: nothing here can flip a PASS to FAIL.
        try:
            ts = time.strftime("%Y-%m-%dT%H-%M-%S")
            raw = bench_export.extract_report(str(run_disk))
            if raw is None:
                if not bench_export.hfsutils_available():
                    print("  history: report not extracted (hfsutils missing — `make e2e-setup`)")
                else:
                    print("  history: report not extracted (no Speedometer text report on the disk "
                          "— Cmd-T save may not have taken)")
                    files = bench_export.list_files(str(run_disk))
                    hits = [f for f in files if bench_export.REPORT_MATCH in f.lower()]
                    if hits:
                        print(f"           files that look related: {hits}")
                return True, None
            rep = bench_export.parse_report(raw)
            hist_root = artifacts / "benchmark-history"
            bench_export.archive_run(
                report=rep, raw_text=raw,
                png=Path(res.result_image) if res.result_image else None,
                history_root=hist_root, timestamp=ts,
                duration_s=res.duration_s, jit_blocks=bench_export.parse_jit_blocks(res.log))
            print(bench_export.format_delta(hist_root / "history.csv"))
            print(f"  archived: {hist_root / ts}")
            return True, rep
        except Exception as e:
            print(f"  history: export skipped ({e})")
            return True, None
    finally:
        shutil.rmtree(work, ignore_errors=True)  # throwaway run-copy; don't accumulate in $TMPDIR


def _single_run() -> int:
    """Resolve assets and run ONE benchmark cycle (one process == one vncdotool reactor)."""
    print(runner.binary_build_info(str(EMULATOR)))  # which binary are we testing, and how fresh?
    assets = config.resolve_assets()
    # Fail fast with a clear, doc-pointing message if an asset is missing (vs a cryptic boot failure).
    try:
        config.require_asset(assets.rom, "rom")
        config.require_asset(assets.disk, "disk")
    except FileNotFoundError as e:
        print(f"FAIL: {e}")
        return 1
    artifacts = HERE / "artifacts"
    artifacts.mkdir(exist_ok=True)
    shutdown_timeout = float(os.environ.get("SS_E2E_SHUTDOWN_TIMEOUT", "60"))
    ok, _ = _run_once(assets=assets, artifacts=artifacts, shutdown_timeout=shutdown_timeout)
    return 0 if ok else 1


def main() -> int:
    # SS_E2E_RUNS>1 runs the benchmark N times for a LESS NOISY signal (median + per-metric CV%).
    runs = max(1, int(os.environ.get("SS_E2E_RUNS", "1")))
    if runs == 1:
        return _single_run()

    # Each run goes in its OWN subprocess: vncdotool's Twisted reactor can't be restarted within a
    # single process (run 2's api.connect() would hang forever), so we re-exec this script per run
    # (each child sees SS_E2E_RUNS=1) and then summarize the rows this batch appended to
    # history.csv — the median (robust to a one-off host-load spike) + each metric's run-to-run CV%.
    hist = HERE / "artifacts" / "benchmark-history" / "history.csv"
    before = len(bench_export.read_history(hist))
    child_env = dict(os.environ, SS_E2E_RUNS="1")
    failures = 0
    for i in range(runs):
        print(f"\n=== run {i + 1}/{runs} ===", flush=True)
        rc = subprocess.run([sys.executable, str(Path(__file__).resolve())], env=child_env).returncode
        failures += 0 if rc == 0 else 1
    new_rows = bench_export.read_history(hist)[before:]
    if len(new_rows) > 1:
        print("\n" + bench_export.format_summary(
            bench_export.summarize(new_rows), label=f"{len(new_rows)} runs"))
    else:
        print(f"\n(only {len(new_rows)} run(s) produced scores — no batch summary)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    code = main()
    # See run_smoke.py: vncdotool's non-daemon reactor thread blocks a normal interpreter exit if a
    # VNC connect failed (close()/api.shutdown() skipped). Force a clean immediate exit.
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(code)
