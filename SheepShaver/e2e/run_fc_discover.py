#!/usr/bin/env python3
"""[ONE-OFF EXAMPLE — not a standing gate; a worked example of the discovery pattern.]

DISCOVERY: boot OS 9 (to Finder) + attach the E2E Apps disk, launch AltiVec Fractal Carbon via
Finder keyboard type-select, and dump its UI (screen / windows / menus + cmd-keys) at each stage so we
can build the real workload drive-steps from facts. Throwaway/iterative; not the final scenario.

Run:  cd SheepShaver/e2e
      SS_E2E_DISK=/Users/Shared/macemu/macos9_fresh.dsk SS_INPUT_LOCKOUT=1 .venv/bin/python run_fc_discover.py
(appsdisk comes from assets/apps.dsk -> e2e-apps.dsk, or SS_E2E_APPSDISK.)
"""
import os
import tempfile
import time
from pathlib import Path

from sse2e import disk, harness, runner, scenario, uidump
from sse2e.vnc import Vnc

HERE = Path(__file__).parent
EMULATOR = HERE.parent / "src" / "Unix" / "SheepShaver"
VNCPORT = 5950


def _dump(tag, dump_dir):
    try:
        snap = uidump.snapshot(str(dump_dir), timeout=10.0)
    except Exception as e:
        print(f"--- {tag}: snapshot unavailable: {e}", flush=True)
        return None
    print(f"\n===== {tag} =====", flush=True)
    print(snap.render(), flush=True)
    if snap.menu_bar:
        for m in snap.menu_bar.menus:
            items = ", ".join(f"{it.text!r}{'=⌘'+it.cmd_key if it.cmd_key else ''}"
                              for it in m.items) or "(none)"
            print(f"   menu {m.title!r}: {items}", flush=True)
    return snap


def main() -> int:
    assets = harness.check_preconditions(need_disk=True)
    if assets is None:
        return 1
    if not assets.appsdisk:
        print("FAIL: no apps disk (set SS_E2E_APPSDISK or assets/apps.dsk)")
        return 1

    work = Path(tempfile.mkdtemp(prefix="ss-fc-"))
    dump_dir = work / "ui"
    dump_dir.mkdir()
    os.environ["SS_UI_DUMP_DIR"] = str(dump_dir)

    boot_copy = disk.copy_pristine(Path(assets.disk), work)
    apps_copy = disk.copy_pristine(Path(assets.appsdisk), work)
    prefs = disk.render_prefs(
        HERE / "config" / "test.prefs.template", work / "test.prefs",
        rom=assets.rom, disk=str(boot_copy),
        appsdisk_line=disk.appsdisk_line(str(apps_copy)), vncport=VNCPORT,
    )
    problem = runner.preflight(str(boot_copy), str(apps_copy))
    if problem:
        print(f"FAIL: preflight — {problem}")
        return 1

    emu = runner.Runner(argv=[str(EMULATOR), "--config", str(prefs)])
    emu.start()
    try:
        ev = scenario._await_boot_ready(emu, 120.0)
        if ev is None:
            print("FAIL: boot timed out (no [BOOT] idle)")
            print(emu.log_text()[-1800:])
            return 1
        scenario._await_ready(emu, 60.0)
        time.sleep(3.0)

        vnc = Vnc(port=VNCPORT)
        snap = _dump("settled Finder (before launch)", dump_dir)
        # figure out a safe empty-desktop click point from the screen size
        scr = (snap.raw.get("screen") if snap else None) or {"width": 800, "height": 600}
        ex, ey = scr["width"] // 2, scr["height"] - 30  # bottom-center is empty desktop

        print(f"\n>>> click empty desktop ({ex},{ey}); type-select 'E2E' volume; Cmd-O", flush=True)
        vnc.click(ex, ey); time.sleep(1.0)
        vnc.type_text("E2E"); time.sleep(1.2)
        vnc.key("super-o"); time.sleep(4.0)
        _dump("after open E2E Apps volume", dump_dir)

        print("\n>>> type-select 'AltiVec' app; Cmd-O to launch", flush=True)
        vnc.type_text("AltiVec"); time.sleep(1.2)
        vnc.key("super-o")
        # wait for Fractal Carbon to come up (app-change signal), up to ~40s
        if scenario._await_app(emu, "Fractal", 40.0) if hasattr(scenario, "_await_app") else True:
            pass
        time.sleep(8.0)
        _dump("AltiVec Fractal Carbon — LAUNCHED (its UI)", dump_dir)

        # capture a screenshot of whatever's on screen for the human record
        try:
            shot = HERE / "artifacts"; shot.mkdir(exist_ok=True)
            vnc.capture(str(shot / "fc_discover.png"))
            print(f"\nscreenshot -> {shot/'fc_discover.png'}", flush=True)
        except Exception as e:
            print(f"screenshot failed: {e}", flush=True)
        vnc.close()
        print("\n[discover] done — UI dumped above. Review to build the workload drive-steps.", flush=True)
        return 0
    finally:
        try:
            emu.request_shutdown(); emu.wait(15.0)
        finally:
            emu.terminate()


if __name__ == "__main__":
    harness.hard_exit(main())
