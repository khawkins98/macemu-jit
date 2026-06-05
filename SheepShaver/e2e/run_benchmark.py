#!/usr/bin/env python3
"""Benchmark entry point (ROADMAP A5 / P2).

Boots the Mac OS 9 + Speedometer disk (Speedometer auto-launches from Startup Items), drives the
full Speedometer suite over VNC, captures the results screenshot + the emulator log, and shuts
down cleanly via the host->guest hook. Requires a logged-in macOS GUI session (SDL window).
"""
import sys
import os
import tempfile
from pathlib import Path

from sse2e import config, disk, runner, scenario

HERE = Path(__file__).parent
EMULATOR = HERE.parent / "src" / "Unix" / "SheepShaver"
VNCPORT = 5950


def main() -> int:
    assets = config.resolve_assets()
    # Fail fast with a clear, doc-pointing message if an asset is missing (vs a cryptic boot failure).
    try:
        config.require_asset(assets.rom, "rom")
        config.require_asset(assets.disk, "disk")
    except FileNotFoundError as e:
        print(f"FAIL: {e}")
        return 1
    # Pre-flight: kill+reap strays and confirm the disk master is free, so a stray instance holding
    # it can't make this run boot to the "?" no-boot-disk icon.
    problem = runner.preflight(assets.disk)
    if problem:
        print(f"FAIL: preflight — {problem}")
        return 1
    work = Path(tempfile.mkdtemp(prefix="ss-e2e-bench-"))
    # Instant APFS clonefile copy so the master stays pristine.
    run_disk = disk.copy_pristine(Path(assets.disk), work)
    prefs = disk.render_prefs(
        HERE / "config" / "test.prefs.template",
        work / "bench.prefs",
        rom=assets.rom,
        disk=str(run_disk),
        vncport=VNCPORT,
    )
    artifacts = HERE / "artifacts"
    artifacts.mkdir(exist_ok=True)
    res = scenario.run_benchmark(
        emulator=str(EMULATOR),
        prefs=str(prefs),
        vncport=VNCPORT,
        artifact_dir=artifacts,
    )
    print(f"{'PASS' if res.ok else 'FAIL'}: {res.reason}")
    if res.result_image:
        print(f"  results image: {res.result_image}")
    print(f"  emulator log:  {artifacts / 'benchmark-emulator.log'}")
    return 0 if res.ok else 1


if __name__ == "__main__":
    code = main()
    # See run_smoke.py: vncdotool's non-daemon reactor thread blocks a normal interpreter exit if a
    # VNC connect failed (close()/api.shutdown() skipped). Force a clean immediate exit.
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(code)
