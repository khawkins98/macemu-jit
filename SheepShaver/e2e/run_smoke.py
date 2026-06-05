#!/usr/bin/env python3
"""P1 smoke entry point.

Boots SheepShaver in the repo-tracked isolated config, waits for the deterministic
[BOOT] idle signal, drives Special > Shut Down over VNC, and asserts a clean exit.

Requires a logged-in macOS GUI session (SDL needs a live WindowServer). Asset paths
(ROM/disk) come from SS_E2E_ROM / SS_E2E_DISK or local-dev defaults (see sse2e/config.py).
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
    runner.kill_strays()  # only one emulator instance at a time
    assets = config.resolve_assets()
    medium = os.environ.get("SS_E2E_MEDIUM", "iso")  # "iso" (read-only, default) or "disk"
    work = Path(tempfile.mkdtemp(prefix="ss-e2e-"))
    if medium == "disk":
        # Writable disk boot: copy the pristine master per run (instant APFS clonefile) so the
        # master is never dirtied. Used for the benchmark medium (Mac OS 9 + Speedometer).
        run_disk = disk.copy_pristine(Path(assets.disk), work)
        prefs = disk.render_prefs(
            HERE / "config" / "test.prefs.template",
            work / "test.prefs",
            rom=assets.rom,
            disk=str(run_disk),
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
    sys.exit(main())
