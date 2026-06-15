#!/usr/bin/env python3
"""Real-world-app workload runner (ROADMAP A5 / S4-S5).

Boots a workload boot disk + attaches the apps disk, launches an app by Finder type-select, and
times its render via screenshot/pHash gating (a Carbon/fullscreen app isn't readable via the window
list — see LEARNINGS 2026-06-06), then quits cleanly and appends a per-workload perf-history row.

Usage:  cd SheepShaver/e2e
        SS_INPUT_LOCKOUT=1 .venv/bin/python run_workload.py [workload-name]   # default: fractal-carbon

Disks (override via env):
  SS_E2E_DISK     boot disk     (default: /Users/Shared/macemu/e2e-macos9-workload-boot.dsk — has CarbonLib 1.6)
  SS_E2E_APPSDISK apps volume   (default: /Users/Shared/macemu/e2e-apps.dsk — has the workload apps)
Both are clonefile-copied per run, so the masters stay pristine.
"""
import os
import sys
import tempfile
import time
from pathlib import Path

from sse2e import disk, harness, runner, scenario
from sse2e.workload import WorkloadSpec, append_history, format_delta

HERE = Path(__file__).parent
EMULATOR = HERE.parent / "src" / "Unix" / "SheepShaver"
VNCPORT = 5950
DEFAULT_BOOT = "/Users/Shared/macemu/e2e-macos9-workload-boot.dsk"
DEFAULT_APPS = "/Users/Shared/macemu/e2e-apps.dsk"

# Workload library. Add real apps here as they're installed onto the apps disk.
WORKLOADS = {
    # AltiVec Fractal Carbon (OS 9, Carbon): self-contained, zero-license, renders a fractal fullscreen
    # with a built-in scalar-vs-AltiVec path. Launched from the "E2E" volume by its "AltiVec" prefix.
    "fractal-carbon": WorkloadSpec(
        name="fractal-carbon", volume="E2E", app="AltiVec", app_signal="Fractal",
        launch_diverge=12, launch_timeout=45.0,
        poll_interval=3.0, stable_threshold=6, stable_frames=2, max_render_s=45.0,
    ),
}


def main(argv) -> int:
    name = argv[1] if len(argv) > 1 else "fractal-carbon"
    spec = WORKLOADS.get(name)
    if spec is None:
        print(f"FAIL: unknown workload {name!r} (known: {', '.join(WORKLOADS)})")
        return 1

    # SS_E2E_TIMEOUT_SCALE: multiply launch/render/quit timeouts (default 1.0, no change).
    # Useful when the guest runs slower than normal — e.g. SS_JIT_PROFILE adds per-block
    # counter overhead (~2x), which otherwise trips the launch gate on a profiled run.
    _scale = float(os.environ.get("SS_E2E_TIMEOUT_SCALE", "1"))
    if _scale != 1.0:
        from dataclasses import replace
        spec = replace(spec, launch_timeout=spec.launch_timeout * _scale,
                       max_render_s=spec.max_render_s * _scale,
                       quit_timeout=spec.quit_timeout * _scale)
        print(f"  [workload] timeouts x{_scale} (launch={spec.launch_timeout:.0f}s "
              f"render={spec.max_render_s:.0f}s quit={spec.quit_timeout:.0f}s)")

    assets = harness.check_preconditions()       # ROM check; disks are explicit paths below
    if assets is None:
        return 1
    boot = os.environ.get("SS_E2E_DISK", DEFAULT_BOOT)
    apps = os.environ.get("SS_E2E_APPSDISK", DEFAULT_APPS)

    work = Path(tempfile.mkdtemp(prefix="ss-workload-"))
    art = HERE / "artifacts"; art.mkdir(exist_ok=True)

    problem = runner.preflight(boot, apps)
    if problem:
        print(f"FAIL: preflight — {problem}")
        return 1
    boot_copy = disk.copy_pristine(Path(boot), work)
    apps_copy = disk.copy_pristine(Path(apps), work)
    prefs = disk.render_prefs(
        HERE / "config" / "test.prefs.template", work / "workload.prefs",
        rom=assets.rom, disk=str(boot_copy),
        appsdisk_line=disk.appsdisk_line(str(apps_copy)), vncport=VNCPORT,
    )

    print(f"  [workload] {name}: boot {Path(boot).name} + {Path(apps).name} (per-run copies)", flush=True)
    res = scenario.run_workload(
        emulator=str(EMULATOR), prefs=str(prefs), vncport=VNCPORT, spec=spec, artifact_dir=art)
    print(f"{'PASS' if res.ok else 'FAIL'}: {res.reason}")
    if res.result_image:
        print(f"  result image: {res.result_image}  (pHash {res.result_phash})")

    # Best-effort perf-history append (never flips the verdict).
    try:
        ts = time.strftime("%Y-%m-%dT%H-%M-%S")
        hist = art / "workload-history" / "history.csv"
        append_history(hist, res, ts)
        print("  " + format_delta(hist, name))
        print(f"  history: {hist}")
    except Exception as e:
        print(f"  history: export skipped ({e})")

    return 0 if res.ok else 1


if __name__ == "__main__":
    harness.hard_exit(main(sys.argv))
