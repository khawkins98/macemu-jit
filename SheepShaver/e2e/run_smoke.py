#!/usr/bin/env python3
"""P1 smoke entry point.

Boots SheepShaver in the repo-tracked isolated config, waits for the deterministic
[BOOT] idle signal, drives Special > Shut Down over VNC, and asserts a clean exit.

Requires a logged-in macOS GUI session (SDL needs a live WindowServer). Asset paths
(ROM/disk) come from SS_E2E_ROM / SS_E2E_DISK or local-dev defaults (see sse2e/config.py).
"""
import os
import tempfile
from pathlib import Path

from sse2e import disk, harness, runner, scenario

HERE = Path(__file__).parent
EMULATOR = HERE.parent / "src" / "Unix" / "SheepShaver"
VNCPORT = 5950


def main() -> int:
    medium = os.environ.get("SS_E2E_MEDIUM", "iso")  # "iso" (read-only, default) or "disk"
    # Build info + fail-fast preflight (build / GUI session / required assets), with actionable
    # messages instead of a cryptic late failure.
    assets = harness.check_preconditions(need_iso=(medium != "disk"), need_disk=(medium == "disk"))
    if assets is None:
        return 1
    boot_image = assets.disk if medium == "disk" else assets.iso
    work = Path(tempfile.mkdtemp(prefix="ss-e2e-"))
    if medium == "disk":
        # Writable disk boot: copy the pristine master per run (instant APFS clonefile) so the
        # master is never dirtied. Used for the benchmark medium (Mac OS 9 + Speedometer).
        run_disk = disk.copy_pristine(Path(assets.disk), work)
        apps_line = ""
        if assets.appsdisk:
            apps_copy = disk.copy_pristine(Path(assets.appsdisk), work)
            apps_line = disk.appsdisk_line(str(apps_copy))
        prefs = disk.render_prefs(
            HERE / "config" / "test.prefs.template",
            work / "test.prefs",
            rom=assets.rom,
            disk=str(run_disk),
            appsdisk_line=apps_line,
            vncport=VNCPORT,
        )
    else:
        # Read-only ISO boot (default): no pristine-copy — the ISO can't be dirtied, so it's
        # stable and reproducible.
        prefs = disk.render_prefs(
            HERE / "config" / "test.prefs.iso.template",
            work / "test.prefs",
            rom=assets.rom,
            cdrom=assets.iso,
            vncport=VNCPORT,
        )
    # Pre-flight: kill+reap strays and confirm the boot image is free, so we never spin up a session
    # that boots to the "?" no-boot-disk icon because a stray instance still owns the disk.
    problem = runner.preflight(boot_image)
    if problem:
        print(f"FAIL: preflight — {problem}")
        return 1
    artifacts = HERE / "artifacts"
    artifacts.mkdir(exist_ok=True)
    res = scenario.run_lifecycle(
        emulator=str(EMULATOR),
        prefs=str(prefs),
        vncport=VNCPORT,
        artifact_dir=artifacts,
    )
    print(f"{'PASS' if res.ok else 'FAIL'}: {res.reason}")
    if not res.ok:
        (artifacts / "fail.log").write_text(res.log)
    return 0 if res.ok else 1


if __name__ == "__main__":
    harness.hard_exit(main())   # os._exit so a stuck vncdotool reactor can't hang the process
